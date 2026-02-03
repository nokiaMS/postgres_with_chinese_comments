/*
 * hyperloglog.h
 *
 * A simple HyperLogLog cardinality estimator implementation
 *
 * Portions Copyright (c) 2014-2025, PostgreSQL Global Development Group
 *
 * Based on Hideaki Ohno's C++ implementation.  The copyright terms of Ohno's
 * original version (the MIT license) follow.
 *
 * src/include/lib/hyperloglog.h
 */

/**
 * 此文件是一个简单的 HyperLogLog 基数估计算法的实现。
 */

/*
 * Copyright (c) 2013 Hideaki Ohno <hide.o.j55{at}gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the 'Software'), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED 'AS IS', WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef HYPERLOGLOG_H
#define HYPERLOGLOG_H

/*
 * HyperLogLog is an approximate technique for computing the number of distinct
 * entries in a set.  Importantly, it does this by using a fixed amount of
 * memory.  See the 2007 paper "HyperLogLog: the analysis of a near-optimal
 * cardinality estimation algorithm" for more.
 *
 * hyperLogLogState
 *
 *		registerWidth		register width, in bits ("k")
 *		nRegisters			number of registers
 *		alphaMM				alpha * m ^ 2 (see initHyperLogLog())
 *		hashesArr			array of hashes
 *		arrSize				size of hashesArr
 */
/**
 * HyperLogLog 是一种用于计算集合中不同条目数量的近似数量的技术。
 * 重要的是，它使用固定量的内存来实现这一点。
 * 参见 2007 年的论文 "HyperLogLog: the analysis of a near-optimal cardinality estimation algorithm"。
 */
typedef struct hyperLogLogState
{
	uint8		registerWidth;
	Size		nRegisters;
	double		alphaMM;

	/**
	 * 哈希值数组。
	 */
	uint8	   *hashesArr;

	/**
	 * 数组大小。
	 */
	Size		arrSize;
} hyperLogLogState;

/**
 * 初始化 HyperLogLog 结构体
 * @param cState
 * @param bwidth
 */
extern void initHyperLogLog(hyperLogLogState *cState, uint8 bwidth);

/**
 * 初始化 HyperLogLog 结构体，指定误差率
 * @param cState
 * @param error
 */
extern void initHyperLogLogError(hyperLogLogState *cState, double error);

/**
 * 向 HyperLogLog 结构体添加哈希值
 * @param cState
 * @param hash
 */
extern void addHyperLogLog(hyperLogLogState *cState, uint32 hash);

/**
 * 估计 HyperLogLog 结构体中的基数
 * @param cState
 * @return
 */
extern double estimateHyperLogLog(hyperLogLogState *cState);

/**
 * 释放 HyperLogLog 结构体的内存
 * @param cState
 */
extern void freeHyperLogLog(hyperLogLogState *cState);

#endif							/* HYPERLOGLOG_H */
