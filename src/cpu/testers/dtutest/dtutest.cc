/*
 * Copyright (c) 2015, Christian Menard
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation are those
 * of the authors and should not be interpreted as representing official policies,
 * either expressed or implied, of the FreeBSD Project.
 */

#include "cpu/testers/dtutest/dtutest.hh"
#include "debug/DtuTest.hh"

unsigned int TESTER_DTU= 0;

bool
DtuTest::CpuPort::recvTimingResp(PacketPtr pkt)
{
    dtutest.completeRequest(pkt);
    return true;
}

void
DtuTest::CpuPort::recvReqRetry()
{
    dtutest.recvRetry();
}

DtuTest::DtuTest(const DtuTestParams *p)
  : MemObject(p),
    tickEvent(this),
    port("port", this),
    masterId(p->system->getMasterId(name())),
    atomic(p->system->isAtomicMode()),
    retryPkt(nullptr)
{
    id = TESTER_DTU++;

    // kick things into action
    schedule(tickEvent, curTick());
}

BaseMasterPort &
DtuTest::getMasterPort(const std::string& if_name, PortID idx)
{
    if (if_name == "port")
        return port;
    else
        return MemObject::getMasterPort(if_name, idx);
}

bool
DtuTest::sendPkt(PacketPtr pkt)
{
    DPRINTF(DtuTest, "Send %s %s request at address 0x%x\n",
                     atomic ? "atomic" : "timed",
                     pkt->isWrite() ? "write" : "read",
                     pkt->getAddr());

    if (atomic)
    {
        port.sendAtomic(pkt);
        completeRequest(pkt);
    }
    else
    {
        bool retry = !port.sendTimingReq(pkt);

        if (retry)
        {
            retryPkt = pkt;
            return false;
        }
    }

    return true;
}

void
DtuTest::completeRequest(PacketPtr pkt)
{
    Request* req = pkt->req;

    DPRINTF(DtuTest, "Completing %s at address %x %s\n",
            pkt->isWrite() ? "write" : "read",
            req->getPaddr(),
            pkt->isError() ? "error" : "success");

    const uint8_t *pkt_data = pkt->getConstPtr<uint8_t>();

    if (pkt->isError())
    {
        warn("%s access failed at %#x\n",
             pkt->isWrite() ? "Write" : "Read", req->getPaddr());
    }
    else
    {
        if (pkt->isRead())
        {
            DPRINTF(DtuTest, "%s: read of %x @ cycle %d returns %x\n",
                             name(),
                             req->getPaddr(),
                             curTick(),
                             pkt_data[0]);
        }
    }

    delete pkt->req;

    // the packet will delete the data
    delete pkt;
}

void
DtuTest::recvRetry()
{
    assert(retryPkt);
    if (port.sendTimingReq(retryPkt))
    {
        DPRINTF(DtuTest, "Proceeding after successful retry\n");

        retryPkt = nullptr;
        // kick things into action again
        schedule(tickEvent, clockEdge(Cycles(1)));
    }
}

void
DtuTest::tick()
{
    /// use this to count how often this function was called
    static int counter = 0;

    PacketPtr pkt = nullptr;

    // at first,write something into the scratchpad
    if (counter < 4)
    {
        Addr paddr = counter;
        Request::Flags flags;

        auto req = new Request(paddr, 1, flags, masterId);
        req->setThreadContext(id, 0);

        pkt = new Packet(req, MemCmd::WriteReq);
        auto pkt_data = new uint8_t[1];
        pkt->dataDynamic(pkt_data);
        pkt_data[0] = static_cast<uint8_t>(counter);
    }
    // now read these bytes via the DTU
    else if (counter < 8)
    {
        Addr paddr = counter - 4 + 0x10000000;
        Request::Flags flags;

        auto req = new Request(paddr, 1, flags, masterId);
        req->setThreadContext(id, 0);

        pkt = new Packet(req, MemCmd::ReadReq);
        auto pkt_data = new uint8_t[1];
        pkt->dataDynamic(pkt_data);
    }
    // now write something via the dtu
    else if (counter < 12)
    {
        Addr paddr = counter + 0x10000000;
        Request::Flags flags;

        auto req = new Request(paddr, 1, flags, masterId);
        req->setThreadContext(id, 0);

        pkt = new Packet(req, MemCmd::WriteReq);
        auto pkt_data = new uint8_t[1];
        pkt->dataDynamic(pkt_data);
        pkt_data[0] = static_cast<uint8_t>(curCycle());
    }
    // now read these bytes directly from the scratchpad
    else if (counter < 16)
    {
        Addr paddr = counter - 4;
        Request::Flags flags;

        auto req = new Request(paddr, 1, flags, masterId);
        req->setThreadContext(id, 0);

        pkt = new Packet(req, MemCmd::ReadReq);
        auto pkt_data = new uint8_t[1];
        pkt->dataDynamic(pkt_data);
    }

    // Schedule next tick if the packet was successfully send.
    // Otherwise block until a retry is received.
    if(pkt != nullptr && sendPkt(pkt))
        schedule(tickEvent, clockEdge(Cycles(1)));

    counter++;
}

DtuTest*
DtuTestParams::create()
{
    return new DtuTest(this);
}