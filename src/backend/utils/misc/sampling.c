/*-------------------------------------------------------------------------
 *
 * sampling.c
 *	  Relation block sampling routines.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/misc/sampling.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <math.h>

#include "utils/sampling.h"


/*
 * BlockSampler_Init -- prepare for random sampling of blocknumbers
 *
 * BlockSampler provides algorithm for block level sampling of a relation
 * as discussed on pgsql-hackers 2004-04-02 (subject "Large DB")
 * It selects a random sample of samplesize blocks out of
 * the nblocks blocks in the table. If the table has less than
 * samplesize blocks, all blocks are selected.
 *
 * Since we know the total number of blocks in advance, we can use the
 * straightforward Algorithm S from Knuth 3.4.2, rather than Vitter's
 * algorithm.
 *
 * Returns the number of blocks that BlockSampler_Next will return.
 */
/**
 * 此函数用于初始化一个BlockSampler结构体，以便进行块级别的随机采样。
 * @param bs BlockSampler结构体的指针，用于存储采样状态。
 * @param nblocks 要采样的总块数，即表中的块数。
 * @param samplesize 期望的样本大小，即希望从nblocks中随机选择的块数。
 * @param randseed 用于初始化随机数生成器的种子值，以确保采样的随机性。
 * @return 返回BlockSampler_Next函数将要返回的块数，即实际被选中的块数。通常情况下，这将是samplesize，但如果nblocks小于samplesize，则返回nblocks。
 */
BlockNumber
BlockSampler_Init(BlockSampler bs, BlockNumber nblocks, int samplesize,
				  uint32 randseed)
{
	//待测量表的总块数。
	bs->N = nblocks;			/* measured table size */

	/*
	 * If we decide to reduce samplesize for tables that have less or not much
	 * more than samplesize blocks, here is the place to do it.
	 */
	bs->n = samplesize;		//期望的样本大小。
	bs->t = 0;					/* blocks scanned so far */	//当前块号，表示已经扫描的块数。
	bs->m = 0;					/* blocks selected so far */ //已经选择的块数。

	//初始化随机数生成器的状态，以便在采样过程中使用。
	sampler_random_init_state(randseed, &bs->randstate);

	//如果表中的块数小于期望的样本大小，则实际被选中的块数将是表中的块数。
	return Min(bs->n, bs->N);
}

/**
 * 此函数用于检查BlockSampler是否还有更多的块可以被采样。
 * @param bs BlockSampler结构体的指针，表示当前的采样状态。
 * @return 如果还有更多的块可以被采样，则返回true；否则返回false。
 */
bool
BlockSampler_HasMore(BlockSampler bs)
{
	return (bs->t < bs->N) && (bs->m < bs->n);
}

BlockNumber
BlockSampler_Next(BlockSampler bs)
{
	BlockNumber K = bs->N - bs->t;	/* remaining blocks */
	int			k = bs->n - bs->m;	/* blocks still to sample */
	double		p;				/* probability to skip block */
	double		V;				/* random */

	Assert(BlockSampler_HasMore(bs));	/* hence K > 0 and k > 0 */

	if ((BlockNumber) k >= K)
	{
		/* need all the rest */
		bs->m++;
		return bs->t++;
	}

	/*----------
	 * It is not obvious that this code matches Knuth's Algorithm S.
	 * Knuth says to skip the current block with probability 1 - k/K.
	 * If we are to skip, we should advance t (hence decrease K), and
	 * repeat the same probabilistic test for the next block.  The naive
	 * implementation thus requires a sampler_random_fract() call for each
	 * block number.  But we can reduce this to one sampler_random_fract()
	 * call per selected block, by noting that each time the while-test
	 * succeeds, we can reinterpret V as a uniform random number in the range
	 * 0 to p. Therefore, instead of choosing a new V, we just adjust p to be
	 * the appropriate fraction of its former value, and our next loop
	 * makes the appropriate probabilistic test.
	 *
	 * We have initially K > k > 0.  If the loop reduces K to equal k,
	 * the next while-test must fail since p will become exactly zero
	 * (we assume there will not be roundoff error in the division).
	 * (Note: Knuth suggests a "<=" loop condition, but we use "<" just
	 * to be doubly sure about roundoff error.)  Therefore K cannot become
	 * less than k, which means that we cannot fail to select enough blocks.
	 *----------
	 */
	V = sampler_random_fract(&bs->randstate);
	p = 1.0 - (double) k / (double) K;
	while (V < p)
	{
		/* skip */
		bs->t++;
		K--;					/* keep K == N - t */

		/* adjust p to be new cutoff point in reduced range */
		p *= 1.0 - (double) k / (double) K;
	}

	/* select */
	bs->m++;
	return bs->t++;
}

/*
 * These two routines embody Algorithm Z from "Random sampling with a
 * reservoir" by Jeffrey S. Vitter, in ACM Trans. Math. Softw. 11, 1
 * (Mar. 1985), Pages 37-57.  Vitter describes his algorithm in terms
 * of the count S of records to skip before processing another record.
 * It is computed primarily based on t, the number of records already read.
 * The only extra state needed between calls is W, a random state variable.
 *
 * reservoir_init_selection_state computes the initial W value.
 *
 * Given that we've already read t records (t >= n), reservoir_get_next_S
 * determines the number of records to skip before the next record is
 * processed.
 */
/**
 * 此函数用于初始化ReservoirState结构体，以便进行基于水库算法的随机采样。
 * @param rs ReservoirState结构体的指针，用于存储采样状态。
 * @param n 期望的样本大小，即希望从数据集中随机选择的记录数。
 */
void
reservoir_init_selection_state(ReservoirState rs, int n)
{
	/*
	 * Reservoir sampling is not used anywhere where it would need to return
	 * repeatable results so we can initialize it randomly.
	 */
	//初始化随机数生成器的状态，以便在采样过程中使用。
	//我们使用pg_prng_uint32函数来生成一个随机的32位无符号整数，并将其作为种子值传递给sampler_random_init_state函数，
	//以初始化ReservoirState结构体中的randstate成员。
	sampler_random_init_state(pg_prng_uint32(&pg_global_prng_state),
							  &rs->randstate);

	/* Initial value of W (for use when Algorithm Z is first applied) */
	//我们使用sampler_random_fract函数来生成一个在0和1之间均匀分布的随机数，并将其作为初始权重W的值。
	//这个权重W将在后续的采样过程中被更新，以决定是否将当前记录纳入采样集合。
	rs->W = exp(-log(sampler_random_fract(&rs->randstate)) / n);
}

/**
 * 此函数用于根据Vitter的水库采样算法计算下一个要跳过的记录数S。
 * @param rs ReservoirState结构体的指针，表示当前的采样状态。
 * @param t 已经读取的记录数，即当前处理到的数据集中的第t条记录。注意，t应该大于或等于n，因为在水库采样算法中，我们在处理第n条记录之后才开始计算要跳过的记录数。
 * @param n 期望的样本大小，即希望从数据集中随机选择的记录数。
 * @return 返回下一个要跳过的记录数S，即在处理完当前记录后，需要跳过的记录数量，直到下一个被选中的记录为止。
 */
double
reservoir_get_next_S(ReservoirState rs, double t, int n)
{
	double		S;

	/* The magic constant here is T from Vitter's paper */
	if (t <= (22.0 * n))
	{
		/* Process records using Algorithm X until t is large enough */
		double		V,
					quot;

		V = sampler_random_fract(&rs->randstate);	/* Generate V */
		S = 0;
		t += 1;
		/* Note: "num" in Vitter's code is always equal to t - n */
		quot = (t - (double) n) / t;
		/* Find min S satisfying (4.1) */
		while (quot > V)
		{
			S += 1;
			t += 1;
			quot *= (t - (double) n) / t;
		}
	}
	else
	{
		/* Now apply Algorithm Z */
		double		W = rs->W;
		double		term = t - (double) n + 1;

		for (;;)
		{
			double		numer,
						numer_lim,
						denom;
			double		U,
						X,
						lhs,
						rhs,
						y,
						tmp;

			/* Generate U and X */
			U = sampler_random_fract(&rs->randstate);
			X = t * (W - 1.0);
			S = floor(X);		/* S is tentatively set to floor(X) */
			/* Test if U <= h(S)/cg(X) in the manner of (6.3) */
			tmp = (t + 1) / term;
			lhs = exp(log(((U * tmp * tmp) * (term + S)) / (t + X)) / n);
			rhs = (((t + X) / (term + S)) * term) / t;
			if (lhs <= rhs)
			{
				W = rhs / lhs;
				break;
			}
			/* Test if U <= f(S)/cg(X) */
			y = (((U * (t + 1)) / term) * (t + S + 1)) / (t + X);
			if ((double) n < S)
			{
				denom = t;
				numer_lim = term + S;
			}
			else
			{
				denom = t - (double) n + S;
				numer_lim = t + 1;
			}
			for (numer = t + S; numer >= numer_lim; numer -= 1)
			{
				y *= numer / denom;
				denom -= 1;
			}
			W = exp(-log(sampler_random_fract(&rs->randstate)) / n);	/* Generate W in advance */
			if (exp(log(y) / n) <= (t + X) / t)
				break;
		}
		rs->W = W;
	}
	return S;
}


/*----------
 * Random number generator used by sampling
 *----------
 */
/**
 * 用于sampling的随机数生成器初始化函数。
 * @param seed 用于初始化随机数生成器的种子值，以确保采样的随机性。
 * @param randstate 指向pg_prng_state结构体的指针，用于存储随机数生成器的状态。
 */
void
sampler_random_init_state(uint32 seed, pg_prng_state *randstate)
{
	//我们使用pg_prng_seed函数来初始化随机数生成器的状态。这个函数会根据提供的种子值生成一个初始状态，确保随机数序列的随机性和可重复性。
	pg_prng_seed(randstate, (uint64) seed);
}

/* Select a random value R uniformly distributed in (0 - 1) */
/**
 * 此函数用于生成一个在0和1之间均匀分布的随机数R。
 * @param randstate 指向pg_prng_state结构体的指针，表示随机数生成器的当前状态。函数将使用这个状态来生成随机数，并且会更新它以便下次调用时使用。
 * @return 返回一个在0和1之间均匀分布的随机值R。注意，返回值不包括0.0，但可能接近于0.0。
 */
double
sampler_random_fract(pg_prng_state *randstate)
{
	double		res;

	/* pg_prng_double returns a value in [0.0 - 1.0), so we must reject 0.0 */
	do
	{
		//我们调用pg_prng_double函数来生成一个在0.0和1.0之间的随机值。
		//由于pg_prng_double返回的值可能包括0.0，但我们需要一个在(0.0, 1.0)之间的值，因此我们使用一个循环来检查生成的随机值是否为0.0。
		//如果是0.0，我们会继续生成新的随机值，直到得到一个非零的值为止。这确保了返回的随机值R始终在(0.0, 1.0)范围内。
		//即：获得一个随机数。
		res = pg_prng_double(randstate);
	} while (unlikely(res == 0.0));
	return res;
}


/*
 * Backwards-compatible API for block sampling
 *
 * This code is now deprecated, but since it's still in use by many FDWs,
 * we should keep it for awhile at least.  The functionality is the same as
 * sampler_random_fract/reservoir_init_selection_state/reservoir_get_next_S,
 * except that a common random state is used across all callers.
 */
/**
 * 这个部分的代码提供了一个向后兼容的API，用于块级别的随机采样。
 * 这个API已经被标记为过时（deprecated），但由于许多外部数据源（FDWs）仍在使用它，因此我们至少应该保留一段时间。
 * 这个API的功能与sampler_random_fract、reservoir_init_selection_state和reservoir_get_next_S相同，只是所有调用者共享一个公共的随机状态。
 */
static ReservoirStateData oldrs;
static bool oldrs_initialized = false;

double
anl_random_fract(void)
{
	/* initialize if first time through */
	if (unlikely(!oldrs_initialized))
	{
		sampler_random_init_state(pg_prng_uint32(&pg_global_prng_state),
								  &oldrs.randstate);
		oldrs_initialized = true;
	}

	/* and compute a random fraction */
	return sampler_random_fract(&oldrs.randstate);
}

double
anl_init_selection_state(int n)
{
	/* initialize if first time through */
	if (unlikely(!oldrs_initialized))
	{
		sampler_random_init_state(pg_prng_uint32(&pg_global_prng_state),
								  &oldrs.randstate);
		oldrs_initialized = true;
	}

	/* Initial value of W (for use when Algorithm Z is first applied) */
	return exp(-log(sampler_random_fract(&oldrs.randstate)) / n);
}

double
anl_get_next_S(double t, int n, double *stateptr)
{
	double		result;

	oldrs.W = *stateptr;
	result = reservoir_get_next_S(&oldrs, t, n);
	*stateptr = oldrs.W;
	return result;
}
