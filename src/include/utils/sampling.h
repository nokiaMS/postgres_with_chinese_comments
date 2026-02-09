/*-------------------------------------------------------------------------
 *
 * sampling.h
 *	  definitions for sampling functions
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/sampling.h
 *
 *-------------------------------------------------------------------------
 */
/**
 * 此文件定义了用于采样函数的函数和数据结构。
 * 这些函数和数据结构被VACUUM和ANALYZE等功能使用，以从表中随机选择块或行进行分析。
 * 采样算法的实现基于Knuth的算法S和水库采样（即蓄水池采样）
 * 在计算机科学领域，"Algorithm S" 通常指的是高德纳（Donald E. Knuth）在其著作中提出的一种‌随机抽样算法‌，用于从 n 个元素中等概率地随机选取 m 个不重复的元素。
 * 该算法是‌蓄水池抽样‌（Reservoir Sampling）的一种经典实现，特别适用于在数据流或未知总长度的序列中进行随机采样。
 * 其核心思想是：在遍历数据的过程中，以一定的概率决定是否将当前元素纳入采样集合，从而保证最终每个元素被选中的概率相等。
 */
#ifndef SAMPLING_H
#define SAMPLING_H

#include "common/pg_prng.h"
#include "storage/block.h"		/* for typedef BlockNumber */


/* Random generator for sampling code */
/**
 * 用于sampling的随机数生成器初始化函数。
 * @param seed 用于初始化随机数生成器的种子值，以确保采样的随机性。
 * @param randstate 指向pg_prng_state结构体的指针，用于存储随机数生成器的状态。
 */
extern void sampler_random_init_state(uint32 seed,
									  pg_prng_state *randstate);

/**
 * 此函数用于生成一个在0和1之间均匀分布的随机数R。
 * @param randstate 指向pg_prng_state结构体的指针，表示随机数生成器的当前状态。函数将使用这个状态来生成随机数，并且会更新它以便下次调用时使用。
 * @return 返回一个在0和1之间均匀分布的随机值R。注意，返回值不包括0.0，但可能接近于0.0。
 */
extern double sampler_random_fract(pg_prng_state *randstate);

/* Block sampling methods */

/* Data structure for Algorithm S from Knuth 3.4.2 */
/**
 * 此结构体用于实现Knuth算法S的块采样方法。
 * 他包含了采样过程中需要维护的状态信息，包括总块数、期望的样本大小、当前块号、已选择的块数以及随机数生成器的状态。
 */
typedef struct
{
	BlockNumber N;				/* number of blocks, known in advance */ //总块的数量，事先知道
	int			n;				/* desired sample size */ //期望的样本大小
	BlockNumber t;				/* current block number */ //当前块号
	int			m;				/* blocks selected so far */ //已选择的块数
	pg_prng_state randstate;	/* random generator state */ //随机生成器状态
} BlockSamplerData;

typedef BlockSamplerData *BlockSampler; //定义了BlockSamplerData的指针类型BlockSampler，方便后续使用。

/**
 * 此函数用于初始化一个BlockSampler结构体，以便进行块级别的随机采样。
 * @param bs BlockSampler结构体的指针，用于存储采样状态。
 * @param nblocks 要采样的总块数，即表中的块数。
 * @param samplesize 期望的样本大小，即希望从nblocks中随机选择的块数。
 * @param randseed 用于初始化随机数生成器的种子值，以确保采样的随机性。
 * @return 返回BlockSampler_Next函数将要返回的块数，即实际被选中的块数。通常情况下，这将是samplesize，但如果nblocks小于samplesize，则返回nblocks。
 */
extern BlockNumber BlockSampler_Init(BlockSampler bs, BlockNumber nblocks,
                                     int samplesize, uint32 randseed);

/**
 * 此函数用于检查BlockSampler是否还有更多的块可以被采样。
 * @param bs BlockSampler结构体的指针，表示当前的采样状态。
 * @return 如果还有更多的块可以被采样，则返回true；否则返回false。
 */
extern bool BlockSampler_HasMore(BlockSampler bs);

/**
 * 此函数用于从BlockSampler中获取下一个被采样的块号。
 * @param bs BlockSampler结构体的指针，表示当前的采样状态。
 * @return 返回下一个被采样的块号。调用此函数前应先调用BlockSampler_HasMore以确保还有更多的块可以被采样。
 */
extern BlockNumber BlockSampler_Next(BlockSampler bs);

/* Reservoir sampling methods */
/**
 * 此结构体用于实现水库采样（Reservoir Sampling）的方法。(即蓄水池采样)
 * 它包含了采样过程中需要维护的状态信息，包括当前的权重W和随机数生成器的状态。
 */
typedef struct
{
	double		W;  //当前的权重W，表示在采样过程中当前元素被选中的概率。水库采样算法通过更新这个权重来决定是否将当前元素纳入采样集合。
	pg_prng_state randstate;	/* random generator state */ //随机生成器状态，用于在采样过程中生成随机数，以决定是否选择当前元素。
} ReservoirStateData;

/**
 * 定义了ReservoirStateData结构体的指针类型ReservoirState，方便后续使用。
 */
typedef ReservoirStateData *ReservoirState;

extern void reservoir_init_selection_state(ReservoirState rs, int n);
extern double reservoir_get_next_S(ReservoirState rs, double t, int n);

/* Old API, still in use by assorted FDWs */
/* For backwards compatibility, these declarations are duplicated in vacuum.h */

extern double anl_random_fract(void);
extern double anl_init_selection_state(int n);
extern double anl_get_next_S(double t, int n, double *stateptr);

#endif							/* SAMPLING_H */
