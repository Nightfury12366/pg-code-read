/*-------------------------------------------------------------------------
 *
 * hio.c
 *	  POSTGRES heap access method input/output code.
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/hio.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "access/hio.h"
#include "access/htup_details.h"
#include "access/visibilitymap.h"
#include "storage/bufmgr.h"
#include "storage/freespace.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"


/*
 * RelationPutHeapTuple - place tuple at specified page
 *
 * !!! EREPORT(ERROR) IS DISALLOWED HERE !!!  Must PANIC on failure!!!
 *
 * Note - caller must hold BUFFER_LOCK_EXCLUSIVE on the buffer.
 */
void
RelationPutHeapTuple(Relation relation,  // 整张表的关系数据结构
					 Buffer buffer,      // int
					 HeapTuple tuple,    // 堆元组数据结构，参见htup.h
					 bool token)         // ?
{
	Page		pageHeader;
	OffsetNumber offnum;

	/*
	 * A tuple that's being inserted speculatively should already have its
	 * token set.
	 */

    /*
     * //TODO token & speculatively有待考究
     */
	Assert(!token || HeapTupleHeaderIsSpeculative(tuple->t_data));

	/* Add the tuple to the page */
    //根据buffer获取相应的page（页头），返回的其实是char指针
	pageHeader = BufferGetPage(buffer);
    //插入数据,PageAddItem函数上一节已介绍，函数成功返回行偏移
    // tuple->t_data 其实就是HeapTupleHeaderData结构体  /* -> tuple header and data */
	offnum = PageAddItem(pageHeader, (Item) tuple->t_data,
						 tuple->t_len, InvalidOffsetNumber, false, true);
    /*
    输入：
       page-指向Page的指针
       item-指向数据的指针
       size-数据大小
       offsetNumber-数据存储的偏移量，InvalidOffsetNumber表示不指定
       flags-不"覆盖"原数据
       is_heap-Heap数据
     输出：
       OffsetNumber-数据存储实际的偏移量
    */

    //如果不成功，记录日志
	if (offnum == InvalidOffsetNumber)
		elog(PANIC, "failed to add tuple to page");

	/* Update tuple->t_self to the actual position where it was stored */
    //&(tuple->t_self)类型为ItemPointer，亦即行指针（ItemPointerData结构体指针）
    //根据buffer获取块号，把块号和行偏移写入行指针中，为tuple->t_self赋值
    //offnum已经由PageAddItem算出
	ItemPointerSet(&(tuple->t_self), BufferGetBlockNumber(buffer), offnum);

	/*
	 * Insert the correct position into CTID of the stored tuple, too (unless
	 * this is a speculative insertion, in which case the token is held in
	 * CTID field instead)
	 */
	if (!token)
	{
        //获取行指针，ItemId即ItemIdData指针

		ItemId		itemId = PageGetItemId(pageHeader, offnum);
        //获取TupleHeader
		HeapTupleHeader item = (HeapTupleHeader) PageGetItem(pageHeader, itemId);
        //更新TupleHeader中的行指针
        /*
         * 破案了，这里写的是bufpage里的item的t_ctid，
         * gdb里p item->t_ctid是赋值过的，而tuple->t_data->t_ctid是未被赋值的初始值
         */
		item->t_ctid = tuple->t_self;
	}
}

/*
 * Read in a buffer, using bulk-insert strategy if bistate isn't NULL.
 */
static Buffer
ReadBufferBI(Relation relation, BlockNumber targetBlock,
			 BulkInsertState bistate)
{
	Buffer		buffer;

	/* If not bulk-insert, exactly like ReadBuffer */
	if (!bistate)
		return ReadBuffer(relation, targetBlock);//非BulkInsert模式，使用常规方法获取

    //TODO 以下为BI模式
	/* If we have the desired block already pinned, re-pin and return it */
	if (bistate->current_buf != InvalidBuffer)
	{
        //当前buf所对应的block_num就是targetBlock
		if (BufferGetBlockNumber(bistate->current_buf) == targetBlock)
		{
			IncrBufferRefCount(bistate->current_buf);//++当前buffer的ref_count
			return bistate->current_buf;
		}
		/* ... else drop the old buffer */
		ReleaseBuffer(bistate->current_buf);//释放以前的current buffer
		bistate->current_buf = InvalidBuffer;//drop
	}
    //否则根据buffer strategy 重新找一个buffer
	/* Perform a read using the buffer strategy */
	buffer = ReadBufferExtended(relation, MAIN_FORKNUM, targetBlock,
								RBM_NORMAL, bistate->strategy);

	/* Save the selected block as target for future inserts */
	IncrBufferRefCount(buffer);//++buffer的ref_count
	bistate->current_buf = buffer;//更新current_buf

	return buffer;
}

/*
 * For each heap page which is all-visible, acquire a pin on the appropriate
 * visibility map page, if we haven't already got one.
 *
 * buffer2 may be InvalidBuffer, if only one buffer is involved.  buffer1
 * must not be InvalidBuffer.  If both buffers are specified, block1 must
 * be less than block2.
 */
static void
GetVisibilityMapPins(Relation relation, Buffer buffer1, Buffer buffer2,
					 BlockNumber block1, BlockNumber block2,
					 Buffer *vmbuffer1, Buffer *vmbuffer2)
{
	bool		need_to_pin_buffer1;
	bool		need_to_pin_buffer2;

	Assert(BufferIsValid(buffer1));
	Assert(buffer2 == InvalidBuffer || block1 <= block2);

	while (1)
	{
		/* Figure out which pins we need but don't have. */
		need_to_pin_buffer1 = PageIsAllVisible(BufferGetPage(buffer1))
			&& !visibilitymap_pin_ok(block1, *vmbuffer1);
		need_to_pin_buffer2 = buffer2 != InvalidBuffer
			&& PageIsAllVisible(BufferGetPage(buffer2))
			&& !visibilitymap_pin_ok(block2, *vmbuffer2);
		if (!need_to_pin_buffer1 && !need_to_pin_buffer2)
			return;

		/* We must unlock both buffers before doing any I/O. */
		LockBuffer(buffer1, BUFFER_LOCK_UNLOCK);
		if (buffer2 != InvalidBuffer && buffer2 != buffer1)
			LockBuffer(buffer2, BUFFER_LOCK_UNLOCK);

		/* Get pins. */
		if (need_to_pin_buffer1)
			visibilitymap_pin(relation, block1, vmbuffer1);
		if (need_to_pin_buffer2)
			visibilitymap_pin(relation, block2, vmbuffer2);

		/* Relock buffers. */
		LockBuffer(buffer1, BUFFER_LOCK_EXCLUSIVE);
		if (buffer2 != InvalidBuffer && buffer2 != buffer1)
			LockBuffer(buffer2, BUFFER_LOCK_EXCLUSIVE);

		/*
		 * If there are two buffers involved and we pinned just one of them,
		 * it's possible that the second one became all-visible while we were
		 * busy pinning the first one.  If it looks like that's a possible
		 * scenario, we'll need to make a second pass through this loop.
		 */
		if (buffer2 == InvalidBuffer || buffer1 == buffer2
			|| (need_to_pin_buffer1 && need_to_pin_buffer2))
			break;
	}
}

/*
 * Extend a relation by multiple blocks to avoid future contention on the
 * relation extension lock.  Our goal is to pre-extend the relation by an
 * amount which ramps up as the degree of contention ramps up, but limiting
 * the result to some sane overall value.
 */
static void
RelationAddExtraBlocks(Relation relation, BulkInsertState bistate)
{
	BlockNumber blockNum,
				firstBlock = InvalidBlockNumber;
	int			extraBlocks;
	int			lockWaiters;

	/* Use the length of the lock wait queue to judge how much to extend. */
	lockWaiters = RelationExtensionLockWaiterCount(relation);
	if (lockWaiters <= 0)
		return;

	/*
	 * It might seem like multiplying the number of lock waiters by as much as
	 * 20 is too aggressive, but benchmarking revealed that smaller numbers
	 * were insufficient.  512 is just an arbitrary cap to prevent
	 * pathological results.
	 */
	extraBlocks = Min(512, lockWaiters * 20);

	do
	{
		Buffer		buffer;
		Page		page;
		Size		freespace;

		/*
		 * Extend by one page.  This should generally match the main-line
		 * extension code in RelationGetBufferForTuple, except that we hold
		 * the relation extension lock throughout.
		 */
		buffer = ReadBufferBI(relation, P_NEW, bistate);

		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buffer);

		if (!PageIsNew(page))
			elog(ERROR, "page %u of relation \"%s\" should be empty but is not",
				 BufferGetBlockNumber(buffer),
				 RelationGetRelationName(relation));

		PageInit(page, BufferGetPageSize(buffer), 0);

		/*
		 * We mark all the new buffers dirty, but do nothing to write them
		 * out; they'll probably get used soon, and even if they are not, a
		 * crash will leave an okay all-zeroes page on disk.
		 */
		MarkBufferDirty(buffer);

		/* we'll need this info below */
		blockNum = BufferGetBlockNumber(buffer);
		freespace = PageGetHeapFreeSpace(page);

		UnlockReleaseBuffer(buffer);

		/* Remember first block number thus added. */
		if (firstBlock == InvalidBlockNumber)
			firstBlock = blockNum;

		/*
		 * Immediately update the bottom level of the FSM.  This has a good
		 * chance of making this page visible to other concurrently inserting
		 * backends, and we want that to happen without delay.
		 */
		RecordPageWithFreeSpace(relation, blockNum, freespace);
	}
	while (--extraBlocks > 0);

	/*
	 * Updating the upper levels of the free space map is too expensive to do
	 * for every block, but it's worth doing once at the end to make sure that
	 * subsequent insertion activity sees all of those nifty free pages we
	 * just inserted.
	 */
	FreeSpaceMapVacuumRange(relation, firstBlock, blockNum + 1);
}

/*
 * RelationGetBufferForTuple
 *
 *	Returns pinned and exclusive-locked buffer of a page in given relation
 *	with free space >= given len.
 *
 *	If otherBuffer is not InvalidBuffer, then it references a previously
 *	pinned buffer of another page in the same relation; on return, this
 *	buffer will also be exclusive-locked.  (This case is used by heap_update;
 *	the otherBuffer contains the tuple being updated.)
 *
 *	The reason for passing otherBuffer is that if two backends are doing
 *	concurrent heap_update operations, a deadlock could occur if they try
 *	to lock the same two buffers in opposite orders.  To ensure that this
 *	can't happen, we impose the rule that buffers of a relation must be
 *	locked in increasing page number order.  This is most conveniently done
 *	by having RelationGetBufferForTuple lock them both, with suitable care
 *	for ordering.
 *
 *	NOTE: it is unlikely, but not quite impossible, for otherBuffer to be the
 *	same buffer we select for insertion of the new tuple (this could only
 *	happen if space is freed in that page after heap_update finds there's not
 *	enough there).  In that case, the page will be pinned and locked only once.
 *
 *	For the vmbuffer and vmbuffer_other arguments, we avoid deadlock by
 *	locking them only after locking the corresponding heap page, and taking
 *	no further lwlocks while they are locked.
 *
 *	We normally use FSM to help us find free space.  However,
 *	if HEAP_INSERT_SKIP_FSM is specified, we just append a new empty page to
 *	the end of the relation if the tuple won't fit on the current target page.
 *	This can save some cycles when we know the relation is new and doesn't
 *	contain useful amounts of free space.
 *
 *	当我们知道关系是新的并且不包含有用的空闲空间时，这可以节省一些周期。
 *
 *	HEAP_INSERT_SKIP_FSM is also useful for non-WAL-logged additions to a
 *	relation, if the caller holds exclusive lock and is careful to invalidate
 *	relation's smgr_targblock before the first insertion --- that ensures that
 *	all insertions will occur into newly added pages and not be intermixed
 *	with tuples from other transactions.  That way, a crash can't risk losing
 *	any committed data of other transactions.  (See heap_insert's comments
 *	for additional constraints needed for safe usage of this behavior.)
 *	HEAP_INSERT_SKIP_FSM对于非WAL记录的关系添加也很有用，如果调用方持有独占锁，并且在第一次
 *	插入之前小心地使关系的smgr_targblock无效——这确保了所有插入都将发生在新添加的页面中，而不会
 *	与其他事务的元组混合。这样，崩溃就不会有丢失其他事务的任何已提交数据的风险。（有关安全使用此行
 *	为所需的其他约束，请参阅heap_insert的注释。）
 *
 *	The caller can also provide a BulkInsertState object to optimize many
 *	insertions into the same relation.  This keeps a pin on the current
 *	insertion target page (to save pin/unpin cycles) and also passes a
 *	BULKWRITE buffer selection strategy object to the buffer manager.
 *	Passing NULL for bistate selects the default behavior.
 *
 *	We always try to avoid filling existing pages further than the fillfactor.
 *	This is OK since this routine is not consulted when updating a tuple and
 *	keeping it on the same page, which is the scenario fillfactor is meant
 *	to reserve space for.
 *	我们总是尽量避免填充现有页面超过填充因子。这是可以的，因为在更新元组并将其保持在同一页面上时，
 *	不会咨询此例程，该场景填充因子的目的是为其保留空间。
 *
 *	ereport(ERROR) is allowed here, so this routine *must* be called
 *	before any (unlogged) changes are made in buffer pool.
 */
/*
输入：
    relation-数据表
    len-需要的空间大小
    otherBuffer-用于update场景，上一次pinned的buffer
    options-处理选项
    bistate-BulkInsert标记
    vmbuffer-第1个vm(visibilitymap)
    vmbuffer_other-用于update场景，上一次pinned的buffer对应的vm(visibilitymap)
    注意:
    otherBuffer这个参数让人觉得困惑，原因是PG的机制使然
    Update时，不是原地更新，而是原数据保留（更新xmax），新数据插入
    原数据&新数据如果在不同Block中，锁定Block的时候可能会出现Deadlock
    举个例子：Session A更新表T的第一行，第一行在Block 0中，新数据存储在Block 2中
              Session B更新表T的第二行，第二行在Block 0中，新数据存储在Block 2中
              Block 0/2均要锁定才能完整实现Update操作：
              如果Session A先锁定了Block 2，Session B先锁定了Block 0，
              然后Session A尝试锁定Block 0，Session B尝试锁定Block 2，这时候就会出现死锁
              为了避免这种情况，PG规定锁定时，同一个Relation，按Block的编号顺序锁定，
              如需要锁定0和2，那必须先锁定Block 0，再锁定2
输出：
    为Tuple分配的Buffer，返回一个int
附：
Pinned buffers：means buffers are currently being used,it should not be flushed out.
*/

Buffer
RelationGetBufferForTuple(Relation relation, Size len,
						  Buffer otherBuffer, int options,
						  BulkInsertState bistate,
						  Buffer *vmbuffer, Buffer *vmbuffer_other)
{
	bool		use_fsm = !(options & HEAP_INSERT_SKIP_FSM);//是否使用FSM寻找空闲空间
	Buffer		buffer = InvalidBuffer;//初始化为invalid的buffer
	Page		page;// char*
	Size		pageFreeSpace = 0,//page空闲空间
				saveFreeSpace = 0;//page需要预留的空间
	BlockNumber targetBlock,//目标Block
				otherBlock;//上一次pinned的buffer对应的Block
	bool		needLock;//是否需要上锁

	len = MAXALIGN(len);		/* be conservative *///大小对齐

	/* Bulk insert is not supported for updates, only inserts. */
    //otherBuffer有效，说明是update操作，不支持bi(BulkInsert)
	Assert(otherBuffer == InvalidBuffer || !bistate);

	/*
	 * If we're gonna fail for oversize tuple, do it right away
	 */
	if (len > MaxHeapTupleSize)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("row is too big: size %zu, maximum size %zu",
						len, MaxHeapTupleSize)));

	/* Compute desired extra freespace due to fillfactor option */
    //获取预留空间
	saveFreeSpace = RelationGetTargetPageFreeSpace(relation,
												   HEAP_DEFAULT_FILLFACTOR);
    //update操作,获取上次pinned buffer对应的Block
	if (otherBuffer != InvalidBuffer)
		otherBlock = BufferGetBlockNumber(otherBuffer);
	else
		otherBlock = InvalidBlockNumber;	/* just to keep compiler quiet */

	/*
	 * We first try to put the tuple on the same page we last inserted a tuple
	 * on, as cached in the BulkInsertState or relcache entry.  If that
	 * doesn't work, we ask the Free Space Map to locate a suitable page.
	 * Since the FSM's info might be out of date, we have to be prepared to
	 * loop around and retry multiple times. (To insure this isn't an infinite
	 * loop, we must update the FSM with the correct amount of free space on
	 * each page that proves not to be suitable.)  If the FSM has no record of
	 * a page with enough free space, we give up and extend the relation.
	 *
	 * When use_fsm is false, we either put the tuple onto the existing target
	 * page or extend the relation.
	 */
	if (len + saveFreeSpace > MaxHeapTupleSize)
	{
        //如果需要的大小+预留空间大于可容纳的最大Tuple大小，不使用FSM，扩展后再尝试
		/* can't fit, don't bother asking FSM */
		targetBlock = InvalidBlockNumber;
		use_fsm = false;
	}
	else if (bistate && bistate->current_buf != InvalidBuffer)//BulkInsert模式
		targetBlock = BufferGetBlockNumber(bistate->current_buf);
	else
		targetBlock = RelationGetTargetBlock(relation);//普通Insert模式

	if (targetBlock == InvalidBlockNumber && use_fsm)//还没有找到合适的BlockNumber，需要使用FSM
	{
		/*
		 * We have no cached target page, so ask the FSM for an initial
		 * target.
		 */
        //使用FSM申请空闲空间=len + saveFreeSpace的块
		targetBlock = GetPageWithFreeSpace(relation, len + saveFreeSpace);

		/*
		 * If the FSM knows nothing of the rel, try the last page before we
		 * give up and extend.  This avoids one-tuple-per-page syndrome during
		 * bootstrapping or in a recently-started system.
		 */
        //申请不到，使用最后一个块，否则扩展或者放弃
		if (targetBlock == InvalidBlockNumber)
		{
			BlockNumber nblocks = RelationGetNumberOfBlocks(relation);

			if (nblocks > 0)
				targetBlock = nblocks - 1;
		}
	}

loop:
	while (targetBlock != InvalidBlockNumber)//已成功获取插入数据的块号
	{
		/*
		 * Read and exclusive-lock the target block, as well as the other
		 * block if one was given, taking suitable care with lock ordering and
		 * the possibility they are the same block.
		 *
		 * If the page-level all-visible flag is set, caller will need to
		 * clear both that and the corresponding visibility map bit.  However,
		 * by the time we return, we'll have x-locked the buffer, and we don't
		 * want to do any I/O while in that state.  So we check the bit here
		 * before taking the lock, and pin the page if it appears necessary.
		 * Checking without the lock creates a risk of getting the wrong
		 * answer, so we'll have to recheck after acquiring the lock.
		 */
		if (otherBuffer == InvalidBuffer)//非Update操作
		{
			/* easy case */
			buffer = ReadBufferBI(relation, targetBlock, bistate);//获取Buffer
            // TODO ReadBufferBI() 和 ReadBuffer() 的区别是啥
			if (PageIsAllVisible(BufferGetPage(buffer)))
                //如果Page全局可见，那么把Page Pin在内存中（Pin的意思是固定/保留）
				visibilitymap_pin(relation, targetBlock, vmbuffer);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);//锁定buffer
		}
		else if (otherBlock == targetBlock)//Update操作，新记录跟原记录在同一个Block中
		{
			/* also easy case */
			buffer = otherBuffer;//就可以直接用原记录的buffer
			if (PageIsAllVisible(BufferGetPage(buffer)))
                //如果Page全局可见，那么把Page Pin在内存中（Pin的意思是固定/保留）
				visibilitymap_pin(relation, targetBlock, vmbuffer);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);//锁定buffer
		}
		else if (otherBlock < targetBlock)//Update操作，原记录所在的Block < 新记录的Block
		{
			/* lock other buffer first */
			buffer = ReadBuffer(relation, targetBlock);
			if (PageIsAllVisible(BufferGetPage(buffer)))
				visibilitymap_pin(relation, targetBlock, vmbuffer);
			LockBuffer(otherBuffer, BUFFER_LOCK_EXCLUSIVE);//优先锁定BlockNumber小的那个
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		}
		else//Update操作，原记录所在的Block > 新记录的Block
		{
			/* lock target buffer first */
			buffer = ReadBuffer(relation, targetBlock);
			if (PageIsAllVisible(BufferGetPage(buffer)))
				visibilitymap_pin(relation, targetBlock, vmbuffer);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);//优先锁定BlockNumber小的那个
			LockBuffer(otherBuffer, BUFFER_LOCK_EXCLUSIVE);
		}

		/*
		 * We now have the target page (and the other buffer, if any) pinned
		 * and locked.  However, since our initial PageIsAllVisible checks
		 * were performed before acquiring the lock, the results might now be
		 * out of date, either for the selected victim buffer, or for the
		 * other buffer passed by the caller.  In that case, we'll need to
		 * give up our locks, go get the pin(s) we failed to get earlier, and
		 * re-lock.  That's pretty painful, but hopefully shouldn't happen
		 * often.
		 *
		 * Note that there's a small possibility that we didn't pin the page
		 * above but still have the correct page pinned anyway, either because
		 * we've already made a previous pass through this loop, or because
		 * caller passed us the right page anyway.
		 *
		 * Note also that it's possible that by the time we get the pin and
		 * retake the buffer locks, the visibility map bit will have been
		 * cleared by some other backend anyway.  In that case, we'll have
		 * done a bit of extra work for no gain, but there's no real harm
		 * done.
		 */
		if (otherBuffer == InvalidBuffer || targetBlock <= otherBlock)
			GetVisibilityMapPins(relation, buffer, otherBuffer,
								 targetBlock, otherBlock, vmbuffer,
								 vmbuffer_other);//Pin VM在内存中
		else
			GetVisibilityMapPins(relation, otherBuffer, buffer,
								 otherBlock, targetBlock, vmbuffer_other,
								 vmbuffer);//Pin VM在内存中

		/*
		 * Now we can check to see if there's enough free space here. If so,
		 * we're done.
		 */
        //从刚才找到的buffer中读出page，BufferGetPage其实是BufferGetBlock的宏，返回void*指针
		page = BufferGetPage(buffer);
        //获取页空闲空间，这个函数里page直接强制转换为PageHeader，然后pd_upper-pd_lower算出空闲空间
		pageFreeSpace = PageGetHeapFreeSpace(page);
		if (len + saveFreeSpace <= pageFreeSpace)//有足够的空间存储数据，返回此Buffer
		{
			/* use this page as future insert target, too */
			RelationSetTargetBlock(relation, targetBlock);
			return buffer;
		}

		/*
		 * Not enough space, so we must give up our page locks and pin (if
		 * any) and prepare to look elsewhere.  We don't care which order we
		 * unlock the two buffers in, so this can be slightly simpler than the
		 * code above.
		 */
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);//解锁
		if (otherBuffer == InvalidBuffer)
			ReleaseBuffer(buffer);//释放
		else if (otherBlock != targetBlock)
		{
			LockBuffer(otherBuffer, BUFFER_LOCK_UNLOCK);//解锁另一个
			ReleaseBuffer(buffer);//释放另一个
		}

		/* Without FSM, always fall out of the loop and extend */
		if (!use_fsm)//不使用FSM定位空闲空间，跳出循环，执行扩展
			break;

		/*
		 * Update FSM as to condition of this page, and ask for another page
		 * to try.
		 */
        //使用FSM获取下一个备选的Block
        //注意：如果全部扫描后发现没有满足条件的Block，targetBlock = InvalidBlockNumber，跳出循环
		targetBlock = RecordAndGetPageWithFreeSpace(relation,
													targetBlock,
													pageFreeSpace,
													len + saveFreeSpace);
	}

    //没有获取满足条件的Block，扩展表
	/*
	 * Have to extend the relation.
	 *
	 * We have to use a lock to ensure no one else is extending the rel at the
	 * same time, else we will both try to initialize the same new page.  We
	 * can skip locking for new or temp relations, however, since no one else
	 * could be accessing them.
	 */
	needLock = !RELATION_IS_LOCAL(relation);//新创建的数据表或者临时表，无需Lock

	/*
	 * If we need the lock but are not able to acquire it immediately, we'll
	 * consider extending the relation by multiple blocks at a time to manage
	 * contention on the relation extension lock.  However, this only makes
	 * sense if we're using the FSM; otherwise, there's no point.
	 */
	if (needLock)//需要锁定
	{
		if (!use_fsm)
			LockRelationForExtension(relation, ExclusiveLock);
		else if (!ConditionalLockRelationForExtension(relation, ExclusiveLock))
		{
			/* Couldn't get the lock immediately; wait for it. */
			LockRelationForExtension(relation, ExclusiveLock);

			/*
			 * Check if some other backend has extended a block for us while
			 * we were waiting on the lock.
			 */
            //如有其它进程扩展了数据表，那么可以成功获取满足条件的targetBlock
			targetBlock = GetPageWithFreeSpace(relation, len + saveFreeSpace);

			/*
			 * If some other waiter has already extended the relation, we
			 * don't need to do so; just use the existing freespace.
			 */
			if (targetBlock != InvalidBlockNumber)
			{
				UnlockRelationForExtension(relation, ExclusiveLock);
				goto loop;
			}

			/* Time to bulk-extend. */
            //其它进程没有扩展
            //Just extend it!
			RelationAddExtraBlocks(relation, bistate);
		}
	}

	/*
	 * In addition to whatever extension we performed above, we always add at
	 * least one block to satisfy our own request.
	 *
	 * XXX This does an lseek - rather expensive - but at the moment it is the
	 * only way to accurately determine how many blocks are in a relation.  Is
	 * it worth keeping an accurate file length in shared memory someplace,
	 * rather than relying on the kernel to do it for us?
	 */
    //扩展表后，New Page！
	buffer = ReadBufferBI(relation, P_NEW, bistate);

	/*
	 * We can be certain that locking the otherBuffer first is OK, since it
	 * must have a lower page number.
	 */
	if (otherBuffer != InvalidBuffer)
		LockBuffer(otherBuffer, BUFFER_LOCK_EXCLUSIVE);//otherBuffer的顺序一定在扩展的Block之前，Lock it！

	/*
	 * Now acquire lock on the new page.
	 */
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);//锁定New Page

	/*
	 * Release the file-extension lock; it's now OK for someone else to extend
	 * the relation some more.  Note that we cannot release this lock before
	 * we have buffer lock on the new page, or we risk a race condition
	 * against vacuumlazy.c --- see comments therein.
	 */
	if (needLock)
		UnlockRelationForExtension(relation, ExclusiveLock);//释放扩展锁

	/*
	 * We need to initialize the empty new page.  Double-check that it really
	 * is empty (this should never happen, but if it does we don't want to
	 * risk wiping out valid data).
	 */
	page = BufferGetPage(buffer);//获取相应的Page

	if (!PageIsNew(page))//不是New Page，那一定某个地方搞错了！
		elog(ERROR, "page %u of relation \"%s\" should be empty but is not",
			 BufferGetBlockNumber(buffer),
			 RelationGetRelationName(relation));
    //初始化New Page
	PageInit(page, BufferGetPageSize(buffer), 0);
    //New Page也满足不了要求的大小，报错
	if (len > PageGetHeapFreeSpace(page))
	{
		/* We should not get here given the test at the top */
		elog(PANIC, "tuple is too big: size %zu", len);
	}

	/*
	 * Remember the new page as our target for future insertions.
	 *
	 * XXX should we enter the new page into the free space map immediately,
	 * or just keep it for this backend's exclusive use in the short run
	 * (until VACUUM sees it)?	Seems to depend on whether you expect the
	 * current backend to make more insertions or not, which is probably a
	 * good bet most of the time.  So for now, don't add it to FSM yet.
	 */
    //终于找到了可用于存储数据的Block
	RelationSetTargetBlock(relation, BufferGetBlockNumber(buffer));
    //返回
	return buffer;
}
