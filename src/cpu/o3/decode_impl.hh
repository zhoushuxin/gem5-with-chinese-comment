/*
 * Copyright (c) 2012, 2014 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2004-2006 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Kevin Lim
 */

#ifndef __CPU_O3_DECODE_IMPL_HH__
#define __CPU_O3_DECODE_IMPL_HH__

#include "arch/types.hh"
#include "base/trace.hh"
#include "config/the_isa.hh"
#include "cpu/o3/decode.hh"
#include "cpu/inst_seq.hh"
#include "debug/Activity.hh"
#include "debug/Decode.hh"
#include "debug/O3PipeView.hh"
#include "params/DerivO3CPU.hh"
#include "sim/full_system.hh"

// clang complains about std::set being overloaded with Packet::set if
// we open up the entire namespace std
using std::list;

template<class Impl>
DefaultDecode<Impl>::DefaultDecode(O3CPU *_cpu, DerivO3CPUParams *params)
    : cpu(_cpu),
      renameToDecodeDelay(params->renameToDecodeDelay),
      iewToDecodeDelay(params->iewToDecodeDelay),
      commitToDecodeDelay(params->commitToDecodeDelay),
      fetchToDecodeDelay(params->fetchToDecodeDelay),
      decodeWidth(params->decodeWidth),
      numThreads(params->numThreads)
{
    if (decodeWidth > Impl::MaxWidth)
        fatal("decodeWidth (%d) is larger than compiled limit (%d),\n"
             "\tincrease MaxWidth in src/cpu/o3/impl.hh\n",
             decodeWidth, static_cast<int>(Impl::MaxWidth));

    // @todo: Make into a parameter
    skidBufferMax = (fetchToDecodeDelay + 1) *  params->fetchWidth;
}

template<class Impl>
void
DefaultDecode<Impl>::startupStage()
{
    resetStage();
}

template<class Impl>
void
DefaultDecode<Impl>::resetStage()
{
    _status = Inactive;

    // Setup status, make sure stall signals are clear.
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        decodeStatus[tid] = Idle;

        stalls[tid].rename = false;
    }
}

template <class Impl>
std::string
DefaultDecode<Impl>::name() const
{
    return cpu->name() + ".decode";
}

template <class Impl>
void
DefaultDecode<Impl>::regStats()
{
    decodeIdleCycles
        .name(name() + ".IdleCycles")
        .desc("Number of cycles decode is idle")
        .prereq(decodeIdleCycles);
    decodeBlockedCycles
        .name(name() + ".BlockedCycles")
        .desc("Number of cycles decode is blocked")
        .prereq(decodeBlockedCycles);
    decodeRunCycles
        .name(name() + ".RunCycles")
        .desc("Number of cycles decode is running")
        .prereq(decodeRunCycles);
    decodeUnblockCycles
        .name(name() + ".UnblockCycles")
        .desc("Number of cycles decode is unblocking")
        .prereq(decodeUnblockCycles);
    decodeSquashCycles
        .name(name() + ".SquashCycles")
        .desc("Number of cycles decode is squashing")
        .prereq(decodeSquashCycles);
    decodeBranchResolved
        .name(name() + ".BranchResolved")
        .desc("Number of times decode resolved a branch")
        .prereq(decodeBranchResolved);
    decodeBranchMispred
        .name(name() + ".BranchMispred")
        .desc("Number of times decode detected a branch misprediction")
        .prereq(decodeBranchMispred);
    decodeControlMispred
        .name(name() + ".ControlMispred")
        .desc("Number of times decode detected an instruction incorrectly"
              " predicted as a control")
        .prereq(decodeControlMispred);
    decodeDecodedInsts
        .name(name() + ".DecodedInsts")
        .desc("Number of instructions handled by decode")
        .prereq(decodeDecodedInsts);
    decodeSquashedInsts
        .name(name() + ".SquashedInsts")
        .desc("Number of squashed instructions handled by decode")
        .prereq(decodeSquashedInsts);
}

template<class Impl>
void
DefaultDecode<Impl>::setTimeBuffer(TimeBuffer<TimeStruct> *tb_ptr)
{
    timeBuffer = tb_ptr;

    // Setup wire to write information back to fetch.
    toFetch = timeBuffer->getWire(0);

    // Create wires to get information from proper places in time buffer.
    fromRename = timeBuffer->getWire(-renameToDecodeDelay);
    fromIEW = timeBuffer->getWire(-iewToDecodeDelay);
    fromCommit = timeBuffer->getWire(-commitToDecodeDelay);
}

template<class Impl>
void
DefaultDecode<Impl>::setDecodeQueue(TimeBuffer<DecodeStruct> *dq_ptr)
{
    decodeQueue = dq_ptr;

    // Setup wire to write information to proper place in decode queue.
    toRename = decodeQueue->getWire(0);
}

template<class Impl>
void
DefaultDecode<Impl>::setFetchQueue(TimeBuffer<FetchStruct> *fq_ptr)
{
    fetchQueue = fq_ptr;

    // Setup wire to read information from fetch queue.
    fromFetch = fetchQueue->getWire(-fetchToDecodeDelay);
}

template<class Impl>
void
DefaultDecode<Impl>::setActiveThreads(std::list<ThreadID> *at_ptr)
{
    activeThreads = at_ptr;
}

template <class Impl>
void
DefaultDecode<Impl>::drainSanityCheck() const
{
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        assert(insts[tid].empty());
        assert(skidBuffer[tid].empty());
    }
}

template <class Impl>
bool
DefaultDecode<Impl>::isDrained() const
{
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        if (!insts[tid].empty() || !skidBuffer[tid].empty() ||
                (decodeStatus[tid] != Running && decodeStatus[tid] != Idle))
            return false;
    }
    return true;
}

template<class Impl>
bool
DefaultDecode<Impl>::checkStall(ThreadID tid) const
{
    bool ret_val = false;

    if (stalls[tid].rename) {
        DPRINTF(Decode,"[tid:%i]: Stall fom Rename stage detected.\n", tid);
        ret_val = true;
    }

    return ret_val;
}

template<class Impl>
inline bool
DefaultDecode<Impl>::fetchInstsValid()
{
    return fromFetch->size > 0;
}

template<class Impl>
bool
DefaultDecode<Impl>::block(ThreadID tid)
{
    DPRINTF(Decode, "[tid:%u]: Blocking.\n", tid);

    // Add the current inputs to the skid buffer so they can be
    // reprocessed when this stage unblocks.
    skidInsert(tid);

    // If the decode status is blocked or unblocking then decode has not yet
    // signalled fetch to unblock. In that case, there is no need to tell
    // fetch to block.
    if (decodeStatus[tid] != Blocked) {
	// 仅对于没有处于blocked状态的线程进行阻塞操作
        // Set the status to Blocked.
        decodeStatus[tid] = Blocked;

        if (toFetch->decodeUnblock[tid]) {
            toFetch->decodeUnblock[tid] = false;
			// 通知fetch阶段decode阶段已经block了，让fetch阶段不再获取新的指令
        } else {
            toFetch->decodeBlock[tid] = true;
            wroteToTimeBuffer = true;
			// 由于本周期确实在block前将fetch阶段的指令插入到skid buffer中
			// 因此设置表示本周对time buffer进行过操作
        }
		// toFetch既有block信号也有unblock信号，两者独立但是不能矛盾

        return true;
    }

    return false;
}

template<class Impl>
bool
DefaultDecode<Impl>::unblock(ThreadID tid)
{
    // Decode is done unblocking only if the skid buffer is empty.
    if (skidBuffer[tid].empty()) {
        DPRINTF(Decode, "[tid:%u]: Done unblocking.\n", tid);
        toFetch->decodeUnblock[tid] = true;
        wroteToTimeBuffer = true;
		// 这个置位仅仅表示decode阶段本周期进行了操作
        decodeStatus[tid] = Running;
        return true;
    }

    DPRINTF(Decode, "[tid:%u]: Currently unblocking.\n", tid);

    return false;
}

template<class Impl>
void
DefaultDecode<Impl>::squash(DynInstPtr &inst, ThreadID tid)
{
    DPRINTF(Decode, "[tid:%i]: [sn:%i] Squashing due to incorrect branch "
            "prediction detected at decode.\n", tid, inst->seqNum);

    // Send back mispredict information.
    toFetch->decodeInfo[tid].branchMispredict = true;
    toFetch->decodeInfo[tid].predIncorrect = true;
    toFetch->decodeInfo[tid].mispredictInst = inst;
    toFetch->decodeInfo[tid].squash = true;
    toFetch->decodeInfo[tid].doneSeqNum = inst->seqNum;
    toFetch->decodeInfo[tid].nextPC = inst->branchTarget();
    toFetch->decodeInfo[tid].branchTaken = inst->pcState().branching();
	// 该变量表示该分支指令实际的taken情况
    toFetch->decodeInfo[tid].squashInst = inst;
	// 设置反向传输信号来通知fetch阶段因为错误分支预测进行squash操作
	// 这里的设置的信息直接由inst本身提供，因此是正确的结果
	
    if (toFetch->decodeInfo[tid].mispredictInst->isUncondCtrl()) {
            toFetch->decodeInfo[tid].branchTaken = true;
    }
	// 对于非分支预测调用branching的获得的分支结果是不准确的，需要进行重新设置

    InstSeqNum squash_seq_num = inst->seqNum;

    // Might have to tell fetch to unblock.
    if (decodeStatus[tid] == Blocked ||
        decodeStatus[tid] == Unblocking) {
        toFetch->decodeUnblock[tid] = 1;
    }
	// squash操作会导致decode阶段阻塞状态解除

    // Set status to squashing.
    decodeStatus[tid] = Squashing;
	// 状态更新为Squashing表示正在进行squash

    for (int i=0; i<fromFetch->size; i++) {
        if (fromFetch->insts[i]->threadNumber == tid &&
            fromFetch->insts[i]->seqNum > squash_seq_num) {
            fromFetch->insts[i]->setSquashed();
        }
    }
	// 当前周期从fetch阶段获得所有指令都将被设置为squashed状态

    // Clear the instruction list and skid buffer in case they have any
    // insts in them.
    while (!insts[tid].empty()) {
        insts[tid].pop();
    }

    while (!skidBuffer[tid].empty()) {
        skidBuffer[tid].pop();
    }

    // Squash instructions up until this one
    cpu->removeInstsUntil(squash_seq_num, tid);
	// 将需要squash的指令从cpu主题的instList中删除
}

template<class Impl>
unsigned
DefaultDecode<Impl>::squash(ThreadID tid)
{
    DPRINTF(Decode, "[tid:%i]: Squashing.\n",tid);

    if (decodeStatus[tid] == Blocked ||
        decodeStatus[tid] == Unblocking) {
        if (FullSystem) {
            toFetch->decodeUnblock[tid] = 1;
        } else {
            // In syscall emulation, we can have both a block and a squash due
            // to a syscall in the same cycle.  This would cause both signals
            // to be high.  This shouldn't happen in full system.
            // @todo: Determine if this still happens.
			
			// 在非FS模式下，模拟系统调用将会导致在同一个周期产生squash和block
			// 因此这里需要对于block信号进行额外的处理，将block信号清除
			// block信号是为了FS模式下的系统调用设置的，在此处是多余的
            if (toFetch->decodeBlock[tid])
                toFetch->decodeBlock[tid] = 0;
            else
                toFetch->decodeUnblock[tid] = 1;
        }
    }
	// 设置发送给fetch阶段的unblock信号为true，并清除block信号

    // Set status to squashing.
    decodeStatus[tid] = Squashing;

    // Go through incoming instructions from fetch and squash them.
    unsigned squash_count = 0;

    for (int i=0; i<fromFetch->size; i++) {
        if (fromFetch->insts[i]->threadNumber == tid) {
            fromFetch->insts[i]->setSquashed();
            squash_count++;
        }
    }

    // Clear the instruction list and skid buffer in case they have any
    // insts in them.
    while (!insts[tid].empty()) {
        insts[tid].pop();
    }

    while (!skidBuffer[tid].empty()) {
        skidBuffer[tid].pop();
    }

    return squash_count;
}

template<class Impl>
void
DefaultDecode<Impl>::skidInsert(ThreadID tid)
{
    DynInstPtr inst = NULL;

    while (!insts[tid].empty()) {
        inst = insts[tid].front();

        insts[tid].pop();

        assert(tid == inst->threadNumber);

        skidBuffer[tid].push(inst);

        DPRINTF(Decode,"Inserting [tid:%d][sn:%lli] PC: %s into decode skidBuffer %i\n",
                inst->threadNumber, inst->seqNum, inst->pcState(), skidBuffer[tid].size());
    }

    // @todo: Eventually need to enforce this by not letting a thread
    // fetch past its skidbuffer
    assert(skidBuffer[tid].size() <= skidBufferMax);
	// 如果一个线程insert操作插入指令过多导致skidbuffer溢出，直接退出了-_-#
}

template<class Impl>
bool
DefaultDecode<Impl>::skidsEmpty()
{
    list<ThreadID>::iterator threads = activeThreads->begin();
    list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;
        if (!skidBuffer[tid].empty())
            return false;
    }

    return true;
}

template<class Impl>
void
DefaultDecode<Impl>::updateStatus()
{
    bool any_unblocking = false;

    list<ThreadID>::iterator threads = activeThreads->begin();
    list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (decodeStatus[tid] == Unblocking) {
            any_unblocking = true;
            break;
        }
    }
	// 检查是否存在一个线程退出了阻塞状态

    // Decode will have activity if it's unblocking.
    if (any_unblocking) {
        if (_status == Inactive) {
            _status = Active;

            DPRINTF(Activity, "Activating stage.\n");

            cpu->activateStage(O3CPU::DecodeIdx);
        }
		// 如果存在任何一个退出阻塞状态的线程，便会激活decode阶段
    } else {
        // If it's not unblocking, then decode will not have any internal
        // activity.  Switch it to inactive.
        if (_status == Active) {
            _status = Inactive;
            DPRINTF(Activity, "Deactivating stage.\n");

            cpu->deactivateStage(O3CPU::DecodeIdx);
        }
    }
	// 这里存在一个问题，Running状态将会导致decode阶段被置于inactive状态
}

template <class Impl>
void
DefaultDecode<Impl>::sortInsts()
{
    int insts_from_fetch = fromFetch->size;
    for (int i = 0; i < insts_from_fetch; ++i) {
        insts[fromFetch->insts[i]->threadNumber].push(fromFetch->insts[i]);
    }
}

template<class Impl>
void
DefaultDecode<Impl>::readStallSignals(ThreadID tid)
{
    if (fromRename->renameBlock[tid]) {
        stalls[tid].rename = true;
    }

    if (fromRename->renameUnblock[tid]) {
        assert(stalls[tid].rename);
        stalls[tid].rename = false;
    }
}

template <class Impl>
bool
DefaultDecode<Impl>::checkSignalsAndUpdate(ThreadID tid)
{
    // Check if there's a squash signal, squash if there is.
    // Check stall signals, block if necessary.
    // If status was blocked
    //     Check if stall conditions have passed
    //         if so then go to unblocking
    // If status was Squashing
    //     check if squashing is not high.  Switch to running this cycle.

    // Update the per thread stall statuses.
    readStallSignals(tid);

    // Check squash signals from commit.
    if (fromCommit->commitInfo[tid].squash) {

        DPRINTF(Decode, "[tid:%u]: Squashing instructions due to squash "
                "from commit.\n", tid);

        squash(tid);

        return true;
    }
	// 处理来自commit阶段的squash操作

    if (checkStall(tid)) {
        return block(tid);
    }
	// 如果发现存在有效的stall信号，则将decode阶段阻塞

    if (decodeStatus[tid] == Blocked) {
        DPRINTF(Decode, "[tid:%u]: Done blocking, switching to unblocking.\n",
                tid);

        decodeStatus[tid] = Unblocking;

        unblock(tid);

        return true;
    }
	// 如果decode阶段没有收到有效的stall信号，并且当前decode阶段处于阻塞状态
	// 则将对应线程的decode阶段退出阻塞
	
    if (decodeStatus[tid] == Squashing) {
        // Switch status to running if decode isn't being told to block or
        // squash this cycle.
        DPRINTF(Decode, "[tid:%u]: Done squashing, switching to running.\n",
                tid);

        decodeStatus[tid] = Running;

        return false;
		// 这个地方难道不应该是true吗？？？
    }
	// squash是一个单周期的工作，新的周期将会完成squash操作，并将decode
	// 阶段恢复为Running状态

    // If we've reached this point, we have not gotten any signals that
    // cause decode to change its status.  Decode remains the same as before.
    return false;
}

template<class Impl>
void
DefaultDecode<Impl>::tick()
{
    wroteToTimeBuffer = false;

    bool status_change = false;

    toRenameIndex = 0;

    list<ThreadID>::iterator threads = activeThreads->begin();
    list<ThreadID>::iterator end = activeThreads->end();

    sortInsts();

    //Check stall and squash signals.
    while (threads != end) {
        ThreadID tid = *threads++;

        DPRINTF(Decode,"Processing [tid:%i]\n",tid);
        status_change =  checkSignalsAndUpdate(tid) || status_change;

        decode(status_change, tid);
    }

    if (status_change) {
        updateStatus();
    }

    if (wroteToTimeBuffer) {
        DPRINTF(Activity, "Activity this cycle.\n");

        cpu->activityThisCycle();
    }
}

template<class Impl>
void
DefaultDecode<Impl>::decode(bool &status_change, ThreadID tid)
{
    // If status is Running or idle,
    //     call decodeInsts()
    // If status is Unblocking,
    //     buffer any instructions coming from fetch
    //     continue trying to empty skid buffer
    //     check if stall conditions have passed

    if (decodeStatus[tid] == Blocked) {
        ++decodeBlockedCycles;
    } else if (decodeStatus[tid] == Squashing) {
        ++decodeSquashCycles;
    }
	// 根据状态递增周期统计计数器

    // Decode should try to decode as many instructions as its bandwidth
    // will allow, as long as it is not currently blocked.
    if (decodeStatus[tid] == Running ||
        decodeStatus[tid] == Idle) {
        DPRINTF(Decode, "[tid:%u]: Not blocked, so attempting to run "
                "stage.\n",tid);

        decodeInsts(tid);
    } else if (decodeStatus[tid] == Unblocking) {
        // Make sure that the skid buffer has something in it if the
        // status is unblocking.
        assert(!skidsEmpty());

        // If the status was unblocking, then instructions from the skid
        // buffer were used.  Remove those instructions and handle
        // the rest of unblocking.
        decodeInsts(tid);

        if (fetchInstsValid()) {
            // Add the current inputs to the skid buffer so they can be
            // reprocessed when this stage unblocks.
            skidInsert(tid);
        }

        status_change = unblock(tid) || status_change;
    }
}

template <class Impl>
void
DefaultDecode<Impl>::decodeInsts(ThreadID tid)
{
    // Instructions can come either from the skid buffer or the list of
    // instructions coming from fetch, depending on decode's status.
    int insts_available = decodeStatus[tid] == Unblocking ?
        skidBuffer[tid].size() : insts[tid].size();
	// 如果当前状态属于unblocking，那么从skidBuffer中处理指令
	// 否则从insts队列中处理指令，

    if (insts_available == 0) {
        DPRINTF(Decode, "[tid:%u] Nothing to do, breaking out"
                " early.\n",tid);
        // Should I change the status to idle?
        ++decodeIdleCycles;
        return;
	// 表示没有需要处理的指令，因此当前周期实际上是空闲的
    } else if (decodeStatus[tid] == Unblocking) {
        DPRINTF(Decode, "[tid:%u] Unblocking, removing insts from skid "
                "buffer.\n",tid);
        ++decodeUnblockCycles;
    } else if (decodeStatus[tid] == Running) {
        ++decodeRunCycles;
    }
	// 其他情况则更新周期计数器

    DynInstPtr inst;

    std::queue<DynInstPtr>
        &insts_to_decode = decodeStatus[tid] == Unblocking ?
        skidBuffer[tid] : insts[tid];

    DPRINTF(Decode, "[tid:%u]: Sending instruction to rename.\n",tid);

    while (insts_available > 0 && toRenameIndex < decodeWidth) {
		// 循环遍历整个队列，选择不多于decodeWidth数量的指令发送到rename阶段
        assert(!insts_to_decode.empty());
		// 由于已经计算了总共可以处理的指令数目，此处不会出现empty的情况

        inst = insts_to_decode.front();
        insts_to_decode.pop();
		// 获取并弹出处理队列顶端的指令

        DPRINTF(Decode, "[tid:%u]: Processing instruction [sn:%lli] with "
                "PC %s\n", tid, inst->seqNum, inst->pcState());

        if (inst->isSquashed()) {
            DPRINTF(Decode, "[tid:%u]: Instruction %i with PC %s is "
                    "squashed, skipping.\n",
                    tid, inst->seqNum, inst->pcState());

            ++decodeSquashedInsts;

            --insts_available;

            continue;
        }
		// 跳过被标记为squashed的指令并且更新计数器

        // Also check if instructions have no source registers.  Mark
        // them as ready to issue at any time.  Not sure if this check
        // should exist here or at a later stage; however it doesn't matter
        // too much for function correctness.
        if (inst->numSrcRegs() == 0) {
            inst->setCanIssue();
        }
		// 如果发现该指令不需要等待任何数据，则会提前设置为可以issue的状态
		
        // This current instruction is valid, so add it into the decode
        // queue.  The next instruction may not be valid, so check to
        // see if branches were predicted correctly.
        toRename->insts[toRenameIndex] = inst;
		// 指令被添加到rename阶段的timebuffer中

        ++(toRename->size);
        ++toRenameIndex;
        ++decodeDecodedInsts;
        --insts_available;
		// 更新计数器

#if TRACING_ON
        if (DTRACE(O3PipeView)) {
            inst->decodeTick = curTick() - inst->fetchTick;
        }
		// 如果开启了trace，则会记录该指令的decode的tick位置
		// 用来回指O3的处理器流程
#endif

        // Ensure that if it was predicted as a branch, it really is a
        // branch.
        if (inst->readPredTaken() && !inst->isControl()) {
            panic("Instruction predicted as a branch!");
			// 发现一个非分支指令被预测为分支指令

            ++decodeControlMispred;

            // Might want to set some sort of boolean and just do
            // a check at the end
            squash(inst, inst->threadNumber);
			// 那么会立即开始squash操作，不再添加任何新的指令
			
            break;
        }

        // Go ahead and compute any PC-relative branches.
        // This includes direct unconditional control and
        // direct conditional control that is predicted taken.
        if (inst->isDirectCtrl() &&
           (inst->isUncondCtrl() || inst->readPredTaken()))
        {
		// 这里检查的是预测taken（包括无条件分支）直接分支指令的地址正确性
		// 因为预测不taken的话没有地址可以比较？？？
            ++decodeBranchResolved;

            if (!(inst->branchTarget() == inst->readPredTarg())) {
				// 这里直接检查分支目标和真正的分支目标是否一致
                ++decodeBranchMispred;

                // Might want to set some sort of boolean and just do
                // a check at the end
                squash(inst, inst->threadNumber);
                TheISA::PCState target = inst->branchTarget();

                DPRINTF(Decode, "[sn:%i]: Updating predictions: PredPC: %s\n",
                        inst->seqNum, target);
                //The micro pc after an instruction level branch should be 0
                inst->setPredTarg(target);
				// 和之前的错误处理不同的是，此处会设置正确的分支目标
                break;
            }
        }
    }

    // If we didn't process all instructions, then we will need to block
    // and put all those instructions into the skid buffer.
    if (!insts_to_decode.empty()) {
        block(tid);
    }
	// 如果发现处理了一圈队列中的指令没有处理完毕，那么说明出现了错误的
	// 分支预测，这时候需要进行squash操作，因此会进行阻塞并将其余的指令
	// 存到skid buffer中，有意义吗？？？反正都要被清空

    // Record that decode has written to the time buffer for activity
    // tracking.
    if (toRenameIndex) {
        wroteToTimeBuffer = true;
    }
	// 如果确实向rename阶段发送了至少1条指令，就说明decode阶段确实做了一些事情
}

#endif//__CPU_O3_DECODE_IMPL_HH__
