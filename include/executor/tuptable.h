/*-------------------------------------------------------------------------
 *
 * tuptable.h
 *	  tuple table support stuff
 *
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/tuptable.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TUPTABLE_H
#define TUPTABLE_H

#include "access/htup.h"
#include "access/tupdesc.h"
#include "storage/buf.h"

/*----------
 * The executor stores tuples in a "tuple table" which is a List of
 * independent TupleTableSlots.  There are several cases we need to handle:
 *		1. physical tuple in a disk buffer page
 *		2. physical tuple constructed in palloc'ed memory
 *		3. "minimal" physical tuple constructed in palloc'ed memory
 *		4. "virtual" tuple consisting of Datum/isnull arrays
 *
 * TupleTableSlot需要处理以下情况：
 *      1. 位于磁盘缓冲区页面上的物理元组
 *      2. 在分配的内存中构造的物理元组
 *      3. 在分配的内存中构造的“最小化”的物理元组
 *      4. 由Datum/isnull数组组成的“虚拟”元组
 *
 * The first two cases are similar in that they both deal with "materialized"
 * tuples, but resource management is different.  For a tuple in a disk page
 * we need to hold a pin on the buffer until the TupleTableSlot's reference
 * to the tuple is dropped; while for a palloc'd tuple we usually want the
 * tuple pfree'd when the TupleTableSlot's reference is dropped.
 *
 * 前两种情况是相似的，它们处理的都是“物化”的元组，只是资源管理方式不同。对于在磁盘页上的元组，
 * 需要将其在缓冲区中钉住(Pin)，直到TupleTableSlot上元组的引用被删除；对于在分配的内存中的元组，
 * 通常在TupleTableSlot上元组的引用被删除时释放内存。
 *
 * A "minimal" tuple is handled similarly to a palloc'd regular tuple.
 * At present, minimal tuples never are stored in buffers, so there is no
 * parallel to case 1.  Note that a minimal tuple has no "system columns".
 * (Actually, it could have an OID, but we have no need to access the OID.)
 *
 * 对一个“最小化”的元组的处理方式与在分配的内存中的元组类似。目前，最小化元组不会存储在缓冲区上，
 * 因此没有与情况1相同的情况。
 *
 * A "virtual" tuple is an optimization used to minimize physical data
 * copying in a nest of plan nodes.  Any pass-by-reference Datums in the
 * tuple point to storage that is not directly associated with the
 * TupleTableSlot; generally they will point to part of a tuple stored in
 * a lower plan node's output TupleTableSlot, or to a function result
 * constructed in a plan node's per-tuple econtext.  It is the responsibility
 * of the generating plan node to be sure these resources are not released
 * for as long as the virtual tuple needs to be valid.  We only use virtual
 * tuples in the result slots of plan nodes --- tuples to be copied anywhere
 * else need to be "materialized" into physical tuples.  Note also that a
 * virtual tuple does not have any "system columns".
 *
 * “虚拟”元组是一种优化，用来在计划节点上最小化物理数据的拷贝。任何通过引用传递的元组的数据(Datum)
 * 并不是直接指向其关联的TupleTableSlot；一般它们会指向存储在低层计划节点的输出TupleTableSlot
 * 上的元组的部分，或者指向在计划节点的per-tuple的内存中构造的函数的结果。由计划节点保证这些资源不
 * 会被释放，只要虚拟元组需要有效。虚拟元组只会在计划节点的结果中使用--拷贝到其它任何地方的元组都
 * 需要“物化”成物理元组。需要注意的是虚拟元组没有任何“系统列”。
 *
 * It is also possible for a TupleTableSlot to hold both physical and minimal
 * copies of a tuple.  This is done when the slot is requested to provide
 * the format other than the one it currently holds.  (Originally we attempted
 * to handle such requests by replacing one format with the other, but that
 * had the fatal defect of invalidating any pass-by-reference Datums pointing
 * into the existing slot contents.)  Both copies must contain identical data
 * payloads when this is the case.
 *
 * TupleTableSlot也可能同时持有物理元组和最小元组。这是在slot被请求提供与它持有的元组不同的格式时
 * （如果替换元组的格式，会导致任何指向已存在的slot内容的按引用传递的Datum失效）。
 *
 * The Datum/isnull arrays of a TupleTableSlot serve double duty.  When the
 * slot contains a virtual tuple, they are the authoritative data.  When the
 * slot contains a physical tuple, the arrays contain data extracted from
 * the tuple.  (In this state, any pass-by-reference Datums point into
 * the physical tuple.)  The extracted information is built "lazily",
 * ie, only as needed.  This serves to avoid repeated extraction of data
 * from the physical tuple.
 *
 * TupleTableSlot上的Datum/isnull数组有两个作用。当slot含有虚拟元组时，它们是权威的数据。
 * 当slot含有物理元组时，这个数组包含的是从物理元组提取的数据（在这种情况下，任何按引用传递的
 * Datum都指向物理元组）。提取的信息被延迟构造，只有需要时才会构造，这样避免从物理元组上重复提
 * 取数据。
 *
 * A TupleTableSlot can also be "empty", holding no valid data.  This is
 * the only valid state for a freshly-created slot that has not yet had a
 * tuple descriptor assigned to it.  In this state, tts_isempty must be
 * true, tts_shouldFree false, tts_tuple NULL, tts_buffer InvalidBuffer,
 * and tts_nvalid zero.
 *
 * TupleTableSlot也可以是空的，不持有任何有效数据。这种状态只在刚创建的还没有赋上元组描述符
 * 的slot上有效。在这种状态，tts_isempty必须为true，tts_shouldFree为false，tts_tuple
 * 为NULL，tts_buffer为InvalidBuffer，并且tts_nvalid为零。
 *
 * The tupleDescriptor is simply referenced, not copied, by the TupleTableSlot
 * code.  The caller of ExecSetSlotDescriptor() is responsible for providing
 * a descriptor that will live as long as the slot does.  (Typically, both
 * slots and descriptors are in per-query memory and are freed by memory
 * context deallocation at query end; so it's not worth providing any extra
 * mechanism to do more.  However, the slot will increment the tupdesc
 * reference count if a reference-counted tupdesc is supplied.)
 *
 * TupleTableSlot只简单引用元组描述符，而不复制。ExecSetSlotDescriptor()的调用者负责提供描述符，
 * 并且保证不小于slot的存活时间。
 *
 * When tts_shouldFree is true, the physical tuple is "owned" by the slot
 * and should be freed when the slot's reference to the tuple is dropped.
 *
 * 当tts_shouldFree为true时，物理元组被slot拥有，因而在slot到元组的引用被删除时需要释放
 * 物理元组。
 *
 * If tts_buffer is not InvalidBuffer, then the slot is holding a pin
 * on the indicated buffer page; drop the pin when we release the
 * slot's reference to that buffer.  (tts_shouldFree should always be
 * false in such a case, since presumably tts_tuple is pointing at the
 * buffer page.)
 *
 * 如果tts_buffer不是InvalidBuffer，那么slot钉住了其指示的缓冲区页面；当释放slot上
 * 指向该缓冲区的引用时需要释放缓冲区（在这种情况下，tts_shouldFree应该总是为false）。
 *
 * tts_nvalid indicates the number of valid columns in the tts_values/isnull
 * arrays.  When the slot is holding a "virtual" tuple this must be equal
 * to the descriptor's natts.  When the slot is holding a physical tuple
 * this is equal to the number of columns we have extracted (we always
 * extract columns from left to right, so there are no holes).
 *
 * tts_nvalid说明在tts_values/isnull数组中的有效列的数量。当slot持有虚拟元组时，它必须
 * 等于描述符的natts。当slot持有物理元组时，它等于已提取的列的数量（列总是被从左到右提取，因
 * 此没有空洞）。
 *
 * tts_values/tts_isnull are allocated when a descriptor is assigned to the
 * slot; they are of length equal to the descriptor's natts.
 *
 * tts_nvalid说明在tts_values/isnull数组中的有效列的数量。当slot持有虚拟元组时，它必须等于
 * 描述符的natts。当slot持有物理元组时，它等于已提取的列的数量（列总是被从左到右提取，因此没有空
 * 洞）
 *
 * tts_mintuple must always be NULL if the slot does not hold a "minimal"
 * tuple.  When it does, tts_mintuple points to the actual MinimalTupleData
 * object (the thing to be pfree'd if tts_shouldFreeMin is true).  If the slot
 * has only a minimal and not also a regular physical tuple, then tts_tuple
 * points at tts_minhdr and the fields of that struct are set correctly
 * for access to the minimal tuple; in particular, tts_minhdr.t_data points
 * MINIMAL_TUPLE_OFFSET bytes before tts_mintuple.  This allows column
 * extraction to treat the case identically to regular physical tuples.
 *
 * 如果slot不持有最小化元组，那么tts_mintuple必须为NULL。当slot持有最小化元组时，tts_mintuple
 * 指向实际的MinimalTupleData对象（如果tts_shouldFreeMin为true，则需要释放内存）。如果slot只
 * 有最小化元组而没有普通的物理元组，那么tts_tuple指向tts_minhdr，并且结构体的字段被正确设置了；
 * 这里特别说明，tts_minhdr.t_data指向了tts_mintuple的MINMAL_TUPLE_OFFSET个字节。
 *
 * tts_slow/tts_off are saved state for slot_deform_tuple, and should not
 * be touched by any other code.
 *
 * tts_slow/tts_off为slot_deform_tuple保存了状态，不应被其它代码访问。
 * 元组表存储在EState的es_tupleTable字段中。节点会根据自身需求申请分配TupleTableSlot，
 * 用于存储节点的输出元组、扫描到的元组等。执行完成后会统一释放元组表中的所有元组。执行器定义
 * 了一组对TupleTableSlot的操作接口，包括slot的创建、销毁、访问等。
 * 实现在src/backend/executor/execTuples.c中。
 *----------
 */

/*
 * 在PostgreSQL中，所有记录都存储在元组中，包括系统数据和用户数据。存储模块提供了元组(HeapTuple)的定义和操作接口。
 * 但是这些接口是针对物理元组的，解析和构造的开销较大，不能满足执行器对性能的需求。执行器在进行投影和选择操作时，需要
 * 快速获取元组的属性；在缓存元组时，又希望元组的体积尽可能的小，以节省内存空间。为此，执行器定义了MinmalTuple，去掉
 * 了“系统列(system columns)“（实际上有一个OID，但是不需要访问）。执行器将元组存储在一个元组表(Tuple Table)中，
 * 元组表实际上是一个由单独的TupleTableSlot组成的链表。
 */

typedef struct TupleTableSlot
{
	NodeTag		type;
	bool		tts_isempty;	/* true = slot is empty */
	bool		tts_shouldFree; /* should pfree tts_tuple? */
	bool		tts_shouldFreeMin;	/* should pfree tts_mintuple? */
#define FIELDNO_TUPLETABLESLOT_SLOW 4
	bool		tts_slow;		/* saved state for slot_deform_tuple */
#define FIELDNO_TUPLETABLESLOT_TUPLE 5
	HeapTuple	tts_tuple;		/* physical tuple, or NULL if virtual */
#define FIELDNO_TUPLETABLESLOT_TUPLEDESCRIPTOR 6
	TupleDesc	tts_tupleDescriptor;	/* slot's tuple descriptor */
	MemoryContext tts_mcxt;		/* slot itself is in this context */
	Buffer		tts_buffer;		/* tuple's buffer, or InvalidBuffer */
#define FIELDNO_TUPLETABLESLOT_NVALID 9
	int			tts_nvalid;		/* # of valid values in tts_values */
#define FIELDNO_TUPLETABLESLOT_VALUES 10
	Datum	   *tts_values;		/* current per-attribute values */
#define FIELDNO_TUPLETABLESLOT_ISNULL 11
	bool	   *tts_isnull;		/* current per-attribute isnull flags */
	MinimalTuple tts_mintuple;	/* minimal tuple, or NULL if none */
	HeapTupleData tts_minhdr;	/* workspace for minimal-tuple-only case */
#define FIELDNO_TUPLETABLESLOT_OFF 14
	uint32		tts_off;		/* saved state for slot_deform_tuple */
	bool		tts_fixedTupleDescriptor;	/* descriptor can't be changed */
} TupleTableSlot;

#define TTS_HAS_PHYSICAL_TUPLE(slot)  \
	((slot)->tts_tuple != NULL && (slot)->tts_tuple != &((slot)->tts_minhdr))

//判断TupleTableSlot 是否为Null（包括Empty）
/*
 * TupIsNull -- is a TupleTableSlot empty?
 */
#define TupIsNull(slot) \
	((slot) == NULL || (slot)->tts_isempty)

/* in executor/execTuples.c */
extern TupleTableSlot *MakeTupleTableSlot(TupleDesc desc);
extern TupleTableSlot *ExecAllocTableSlot(List **tupleTable, TupleDesc desc);
extern void ExecResetTupleTable(List *tupleTable, bool shouldFree);
extern TupleTableSlot *MakeSingleTupleTableSlot(TupleDesc tupdesc);
extern void ExecDropSingleTupleTableSlot(TupleTableSlot *slot);
extern void ExecSetSlotDescriptor(TupleTableSlot *slot, TupleDesc tupdesc);
extern TupleTableSlot *ExecStoreTuple(HeapTuple tuple,
			   TupleTableSlot *slot,
			   Buffer buffer,
			   bool shouldFree);
extern TupleTableSlot *ExecStoreMinimalTuple(MinimalTuple mtup,
					  TupleTableSlot *slot,
					  bool shouldFree);
extern TupleTableSlot *ExecClearTuple(TupleTableSlot *slot);
extern TupleTableSlot *ExecStoreVirtualTuple(TupleTableSlot *slot);
extern TupleTableSlot *ExecStoreAllNullTuple(TupleTableSlot *slot);
extern HeapTuple ExecCopySlotTuple(TupleTableSlot *slot);
extern MinimalTuple ExecCopySlotMinimalTuple(TupleTableSlot *slot);
extern HeapTuple ExecFetchSlotTuple(TupleTableSlot *slot);
extern MinimalTuple ExecFetchSlotMinimalTuple(TupleTableSlot *slot);
extern Datum ExecFetchSlotTupleDatum(TupleTableSlot *slot);
extern HeapTuple ExecMaterializeSlot(TupleTableSlot *slot);
extern TupleTableSlot *ExecCopySlot(TupleTableSlot *dstslot,
			 TupleTableSlot *srcslot);

/* in access/common/heaptuple.c */
extern Datum slot_getattr(TupleTableSlot *slot, int attnum, bool *isnull);
extern void slot_getallattrs(TupleTableSlot *slot);
extern void slot_getsomeattrs(TupleTableSlot *slot, int attnum);
extern bool slot_attisnull(TupleTableSlot *slot, int attnum);
extern bool slot_getsysattr(TupleTableSlot *slot, int attnum,
				Datum *value, bool *isnull);
extern void slot_getmissingattrs(TupleTableSlot *slot, int startAttNum, int lastAttNum);

#endif							/* TUPTABLE_H */
