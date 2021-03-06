/*
 * Copyright (c) 2015, Christian Menard
 * Copyright (c) 2015, Nils Asmussen
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation are
 * those of the authors and should not be interpreted as representing official
 * policies, either expressed or implied, of the FreeBSD Project.
 */

#include <iomanip>
#include <sstream>

#include "debug/Dtu.hh"
#include "debug/DtuBuf.hh"
#include "debug/DtuCmd.hh"
#include "debug/DtuPackets.hh"
#include "debug/DtuSysCalls.hh"
#include "debug/DtuPower.hh"
#include "cpu/simple/base.hh"
#include "mem/dtu/dtu.hh"
#include "mem/dtu/msg_unit.hh"
#include "mem/dtu/mem_unit.hh"
#include "mem/dtu/xfer_unit.hh"
#include "mem/page_table.hh"
#include "sim/system.hh"
#include "sim/process.hh"

static const char *cmdNames[] =
{
    "IDLE",
    "SEND",
    "REPLY",
    "READ",
    "WRITE",
    "INC_READ_PTR",
    "WAKEUP_CORE",
};

Dtu::Dtu(DtuParams* p)
  : BaseDtu(p),
    masterId(p->system->getMasterId(name())),
    system(p->system),
    regFile(name() + ".regFile", p->num_endpoints),
    msgUnit(new MessageUnit(*this)),
    memUnit(new MemoryUnit(*this)),
    xferUnit(new XferUnit(*this, p->block_size, p->buf_count, p->buf_size)),
    executeCommandEvent(*this),
    finishCommandEvent(*this),
    cmdInProgress(false),
    memEp(p->memory_ep),
    atomicMode(p->system->isAtomicMode()),
    numEndpoints(p->num_endpoints),
    maxNocPacketSize(p->max_noc_packet_size),
    numCmdEpidBits(p->num_cmd_epid_bits),
    blockSize(p->block_size),
    bufCount(p->buf_count),
    bufSize(p->buf_size),
    registerAccessLatency(p->register_access_latency),
    commandToNocRequestLatency(p->command_to_noc_request_latency),
    startMsgTransferDelay(p->start_msg_transfer_delay),
    transferToMemRequestLatency(p->transfer_to_mem_request_latency),
    transferToNocLatency(p->transfer_to_noc_latency),
    nocToTransferLatency(p->noc_to_transfer_latency)
{
    assert(p->buf_size >= maxNocPacketSize);

    regFile.set(memEp, EpReg::TGT_COREID, p->memory_pe);
    regFile.set(memEp, EpReg::REQ_REM_ADDR, p->memory_offset);
    regFile.set(memEp, EpReg::REQ_REM_SIZE, p->memory_size);
    regFile.set(memEp, EpReg::REQ_FLAGS, READ | WRITE);
}

Dtu::~Dtu()
{
    delete xferUnit;
    delete memUnit;
    delete msgUnit;
}

PacketPtr
Dtu::generateRequest(Addr paddr, Addr size, MemCmd cmd)
{
    Request::Flags flags;

    auto req = new Request(paddr, size, flags, masterId);

    auto pkt = new Packet(req, cmd);
    auto pktData = new uint8_t[size];
    pkt->dataDynamic(pktData);

    return pkt;
}

void
Dtu::freeRequest(PacketPtr pkt)
{
    delete pkt->req;
    delete pkt;
}

Dtu::Command
Dtu::getCommand()
{
    assert(numCmdEpidBits + numCmdOpcodeBits <= sizeof(RegFile::reg_t) * 8);

    using reg_t = RegFile::reg_t;

    /*
     *   COMMAND            0
     * |--------------------|
     * |  epid   |  opcode  |
     * |--------------------|
     */
    reg_t opcodeMask = ((reg_t)1 << numCmdOpcodeBits) - 1;
    reg_t epidMask   = (((reg_t)1 << numCmdEpidBits) - 1) << numCmdOpcodeBits;

    auto reg = regFile.get(CmdReg::COMMAND);

    Command cmd;

    cmd.opcode = static_cast<CommandOpcode>(reg & opcodeMask);

    cmd.epId = (reg & epidMask) >> numCmdOpcodeBits;

    return cmd;
}

void
Dtu::executeCommand()
{
    Command cmd = getCommand();
    if(cmd.opcode == CommandOpcode::IDLE)
        return;

    assert(!cmdInProgress);
    assert(cmd.epId < numEndpoints);

    cmdInProgress = true;

    DPRINTF(DtuCmd, "Starting command %s with EP%d\n",
            cmdNames[static_cast<size_t>(cmd.opcode)], cmd.epId);

    switch (cmd.opcode)
    {
    case CommandOpcode::SEND:
    case CommandOpcode::REPLY:
        msgUnit->startTransmission(cmd);
        break;
    case CommandOpcode::READ:
        memUnit->startRead(cmd);
        break;
    case CommandOpcode::WRITE:
        memUnit->startWrite(cmd);
        break;
    case CommandOpcode::INC_READ_PTR:
        msgUnit->incrementReadPtr(cmd.epId);
        finishCommand();
        break;
    case CommandOpcode::WAKEUP_CORE:
        wakeupCore();
        finishCommand();
        break;
    default:
        // TODO error handling
        panic("Invalid opcode %#x\n", static_cast<RegFile::reg_t>(cmd.opcode));
    }
}

void
Dtu::finishCommand()
{
    Command cmd = getCommand();

    assert(cmdInProgress);

    DPRINTF(DtuCmd, "Finished command %s with EP%d\n",
            cmdNames[static_cast<size_t>(cmd.opcode)], cmd.epId);

    // let the SW know that the command is finished
    regFile.set(CmdReg::COMMAND, 0);

    cmdInProgress = false;
}

void
Dtu::wakeupCore()
{
    if(system->threadContexts.size() == 0)
        return;

    if(system->threadContexts[0]->status() == ThreadContext::Suspended)
    {
        DPRINTF(DtuPower, "Waking up core\n");
        system->threadContexts[0]->activate();
    }
}

void
Dtu::updateSuspendablePin()
{
    if(system->threadContexts.size() == 0)
        return;

    bool pendingMsgs = regFile.get(DtuReg::MSG_CNT) > 0;
    bool hadPending = system->threadContexts[0]->getCpuPtr()->_denySuspend;
    system->threadContexts[0]->getCpuPtr()->_denySuspend = pendingMsgs;
    if(hadPending && !pendingMsgs)
        DPRINTF(DtuPower, "Core can be suspended\n");
}

void
Dtu::sendMemRequest(PacketPtr pkt,
                    unsigned epId,
                    MemReqType type,
                    Cycles delay)
{
    auto senderState = new MemSenderState();
    senderState->epId = epId;
    senderState->mid = pkt->req->masterId();
    senderState->type = type;

    // ensure that this packet has our master id (not the id of a master in a different PE)
    pkt->req->setMasterId(masterId);

    pkt->pushSenderState(senderState);

    if (atomicMode)
    {
        sendAtomicMemRequest(pkt);
        completeMemRequest(pkt);
    }
    else
    {
        schedMemRequest(pkt, clockEdge(delay));
    }
}

void
Dtu::sendNocRequest(NocPacketType type, PacketPtr pkt, Cycles delay, bool functional)
{
    auto senderState = new NocSenderState();
    senderState->packetType = type;

    pkt->pushSenderState(senderState);

    if (functional)
    {
        sendFunctionalNocRequest(pkt);
        completeNocRequest(pkt);
    }
    else if (atomicMode)
    {
        sendAtomicNocRequest(pkt);
        completeNocRequest(pkt);
    }
    else
    {
        schedNocRequest(pkt, clockEdge(delay));
    }
}

void
Dtu::startTransfer(TransferType type,
                   NocAddr targetAddr,
                   Addr sourceAddr,
                   Addr size,
                   PacketPtr pkt,
                   MessageHeader* header,
                   Cycles delay,
                   bool last)
{
    xferUnit->startTransfer(type,
                            targetAddr,
                            sourceAddr,
                            size,
                            pkt,
                            header,
                            delay,
                            last);
}

void
Dtu::completeNocRequest(PacketPtr pkt)
{
    auto senderState = dynamic_cast<NocSenderState*>(pkt->popSenderState());

    if(senderState->packetType == NocPacketType::CACHE_MEM_REQ)
    {
        Addr targetAddr = regs().get(numEndpoints - 1, EpReg::REQ_REM_ADDR);
        Addr reqAddr = NocAddr(pkt->getAddr()).offset - targetAddr;
        pkt->setAddr(reqAddr);
        pkt->req->setPaddr(reqAddr);
        sendCacheMemResponse(pkt);
    }
    else if(senderState->packetType != NocPacketType::CACHE_MEM_REQ_FUNC)
    {
        if (pkt->isWrite())
            memUnit->writeComplete(pkt);
        else if (pkt->isRead())
            memUnit->readComplete(pkt);
        else
            panic("unexpected packet type\n");
    }

    delete senderState;
}

void
Dtu::completeMemRequest(PacketPtr pkt)
{
    assert(!pkt->isError());
    assert(pkt->isResponse());

    auto senderState = dynamic_cast<MemSenderState*>(pkt->popSenderState());

    // set the old master id again
    pkt->req->setMasterId(senderState->mid);

    switch(senderState->type)
    {
    case MemReqType::TRANSFER:
        xferUnit->recvMemResponse(senderState->epId,
                                  pkt->getConstPtr<uint8_t>(),
                                  pkt->getSize(),
                                  pkt->headerDelay,
                                  pkt->payloadDelay);
        break;

    case MemReqType::HEADER:
        msgUnit->recvFromMem(getCommand(), pkt);
        break;
    }

    delete senderState;
    freeRequest(pkt);
}

void
Dtu::handleNocRequest(PacketPtr pkt)
{
    assert(!pkt->isError());

    auto senderState = dynamic_cast<NocSenderState*>(pkt->senderState);

    switch (senderState->packetType)
    {
    case NocPacketType::MESSAGE:
        msgUnit->recvFromNoc(pkt);
        break;
    case NocPacketType::READ_REQ:
    case NocPacketType::WRITE_REQ:
    case NocPacketType::CACHE_MEM_REQ:
        memUnit->recvFromNoc(pkt);
        break;
    case NocPacketType::CACHE_MEM_REQ_FUNC:
        memUnit->recvFunctionalFromNoc(pkt);
        break;
    default:
        panic("Unexpected NocPacketType\n");
    }
}

void
Dtu::handleCpuRequest(PacketPtr pkt)
{
    forwardRequestToRegFile(pkt, true);
}

bool
Dtu::handleCacheMemRequest(PacketPtr pkt, bool functional)
{
    if(pkt->cmd == MemCmd::CleanEvict)
    {
        assert(!pkt->needsResponse());
        DPRINTF(DtuPackets, "Dropping CleanEvict packet\n");
        return true;
    }

    // we don't have cache coherence. so we don't care about invalidate requests
    if(pkt->cmd == MemCmd::InvalidateReq)
        return false;

    unsigned targetCoreId = regs().get(memEp, EpReg::TGT_COREID);
    Addr targetAddr = regs().get(memEp, EpReg::REQ_REM_ADDR);
    Addr remoteSize = regs().get(memEp, EpReg::REQ_REM_SIZE);
    unsigned flags = regs().get(memEp, EpReg::REQ_FLAGS);

    if((pkt->isWrite() && !(flags & WRITE)) ||
      ((pkt->isRead() && !(flags & READ))))
    {
        DPRINTF(Dtu, "Denying %s request @ %p:%lu because of insufficient permissions\n",
                pkt->isRead() ? "read" : "write",
                pkt->getAddr(), pkt->getSize());
        return false;
    }

    if((pkt->getAddr() + pkt->getSize() <= pkt->getAddr()) ||
       (pkt->getAddr() + pkt->getSize() > remoteSize))
    {
        DPRINTF(Dtu, "Denying %s request @ %p:%lu because it's out of bounds (%p..%p)\n",
                pkt->isRead() ? "read" : "write",
                pkt->getAddr(), pkt->getSize(),
                0, remoteSize);
        return false;
    }

    pkt->setAddr(NocAddr(targetCoreId, 0, targetAddr + pkt->getAddr()).getAddr());

    auto type = functional ? Dtu::NocPacketType::CACHE_MEM_REQ_FUNC : Dtu::NocPacketType::CACHE_MEM_REQ;
    sendNocRequest(type, pkt, Cycles(1), functional);

    return true;
}

void
Dtu::forwardRequestToRegFile(PacketPtr pkt, bool isCpuRequest)
{
    Addr oldAddr = pkt->getAddr();

    // Strip the base address to handle requests based on the register address only.
    pkt->setAddr(oldAddr - regFileBaseAddr);

    bool commandWritten = regFile.handleRequest(pkt, isCpuRequest);

    // restore old address
    pkt->setAddr(oldAddr);

    updateSuspendablePin();

    if (!atomicMode)
    {
        /*
         * We handle the request immediatly and do not care about timing. The
         * delay is payed by scheduling the response at some point in the
         * future. Additionaly a write operation on the command register needs
         * to schedule an event that executes this command at a future tick.
         */

        Cycles transportDelay =
            ticksToCycles(pkt->headerDelay + pkt->payloadDelay);

        Tick when = clockEdge(transportDelay + registerAccessLatency);

        pkt->headerDelay = 0;
        pkt->payloadDelay = 0;

        if (isCpuRequest)
            schedCpuResponse(pkt, when);
        else
        {
            schedNocRequestFinished(clockEdge(Cycles(1)));
            schedNocResponse(pkt, when);
        }

        if (commandWritten)
            schedule(executeCommandEvent, when);
    }
    else if (commandWritten)
    {
        executeCommand();
    }
}

Dtu*
DtuParams::create()
{
    return new Dtu(this);
}

void
Dtu::printPacket(PacketPtr pkt) const
{
    DDUMP(DtuPackets, pkt->getPtr<uint8_t>(), pkt->getSize());
}
