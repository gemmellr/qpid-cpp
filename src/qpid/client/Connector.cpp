/*
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *   http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 */
#include "Connector.h"

#include "Bounds.h"
#include "ConnectionImpl.h"
#include "ConnectionSettings.h"
#include "qpid/log/Statement.h"
#include "qpid/sys/Time.h"
#include "qpid/framing/AMQFrame.h"
#include "qpid/sys/AsynchIO.h"
#include "qpid/sys/Dispatcher.h"
#include "qpid/sys/Poller.h"
#include "qpid/Msg.h"

#include <iostream>
#include <boost/bind.hpp>
#include <boost/format.hpp>

namespace qpid {
namespace client {

using namespace qpid::sys;
using namespace qpid::framing;
using boost::format;
using boost::str;

Connector::Connector(ProtocolVersion ver,
                     const ConnectionSettings& settings,
                     ConnectionImpl* cimpl)
    : maxFrameSize(settings.maxFrameSize),
      version(ver), 
      initiated(false),
      closed(true),
      joined(true),
      timeout(0),
      idleIn(0), idleOut(0), 
      timeoutHandler(0),
      shutdownHandler(0),
      writer(maxFrameSize, cimpl),
      aio(0),
      impl(cimpl)
{
    QPID_LOG(debug, "Connector created for " << version);
    settings.configureSocket(socket);
}

Connector::~Connector() {
    close();
}

void Connector::connect(const std::string& host, int port){
    Mutex::ScopedLock l(closedLock);
    assert(closed);
    socket.connect(host, port);
    identifier = str(format("[%1% %2%]") % socket.getLocalPort() % socket.getPeerAddress());
    closed = false;
    poller = Poller::shared_ptr(new Poller);
    aio = new AsynchIO(socket,
                       boost::bind(&Connector::readbuff, this, _1, _2),
                       boost::bind(&Connector::eof, this, _1),
                       boost::bind(&Connector::eof, this, _1),
                       0, // closed
                       0, // nobuffs
                       boost::bind(&Connector::writebuff, this, _1));
    writer.init(identifier, aio);
}

void Connector::init(){
    Mutex::ScopedLock l(closedLock);
    assert(joined);
    ProtocolInitiation init(version);
    writeDataBlock(init);
    joined = false;
    receiver = Thread(this);
}

bool Connector::closeInternal() {
    Mutex::ScopedLock l(closedLock);
    bool ret = !closed;
    if (!closed) {
        closed = true;
        poller->shutdown();
    }
    if (!joined && receiver.id() != Thread::current().id()) {
        joined = true;
        Mutex::ScopedUnlock u(closedLock);
        receiver.join();
    }
    return ret;
}
        
void Connector::close() {
    closeInternal();
}

void Connector::setInputHandler(InputHandler* handler){
    input = handler;
}

void Connector::setShutdownHandler(ShutdownHandler* handler){
    shutdownHandler = handler;
}

OutputHandler* Connector::getOutputHandler(){ 
    return this; 
}

void Connector::send(AMQFrame& frame) {
    writer.handle(frame);
}

void Connector::handleClosed() {
    if (closeInternal() && shutdownHandler)
        shutdownHandler->shutdown();
}

struct Connector::Buff : public AsynchIO::BufferBase {
    Buff(size_t size) : AsynchIO::BufferBase(new char[size], size) {}    
    ~Buff() { delete [] bytes;}
};

Connector::Writer::Writer(uint16_t s, Bounds* b) : maxFrameSize(s), aio(0), buffer(0), lastEof(0), bounds(b)
{
}

Connector::Writer::~Writer() { delete buffer; }

void Connector::Writer::init(std::string id, sys::AsynchIO* a) {
    Mutex::ScopedLock l(lock);
    identifier = id;
    aio = a;
    newBuffer(l);
}
void Connector::Writer::handle(framing::AMQFrame& frame) { 
    Mutex::ScopedLock l(lock);
    frames.push_back(frame);
    if (frame.getEof()) {//or if we already have a buffers worth
        lastEof = frames.size();
        QPID_LOG(debug, "Requesting write: lastEof=" << lastEof);
        aio->notifyPendingWrite();
    }
    QPID_LOG(trace, "SENT " << identifier << ": " << frame);
}

void Connector::Writer::writeOne(const Mutex::ScopedLock& l) {
    assert(buffer);
    QPID_LOG(trace, "Write buffer " << encode.getPosition()
             << " bytes " << framesEncoded << " frames ");    
    framesEncoded = 0;

    buffer->dataStart = 0;
    buffer->dataCount = encode.getPosition();
    aio->queueWrite(buffer);
    newBuffer(l);
}

void Connector::Writer::newBuffer(const Mutex::ScopedLock&) {
    buffer = aio->getQueuedBuffer();
    if (!buffer) buffer = new Buff(maxFrameSize);
    encode = framing::Buffer(buffer->bytes, buffer->byteCount);
    framesEncoded = 0;
}

// Called in IO thread.
void Connector::Writer::write(sys::AsynchIO&) {
    Mutex::ScopedLock l(lock);
    assert(buffer);
    size_t bytesWritten(0);
    for (size_t i = 0; i < lastEof; ++i) {
        AMQFrame& frame = frames[i];
        uint32_t size = frame.size();
        if (size > encode.available()) writeOne(l);
        assert(size <= encode.available());
        frame.encode(encode);
        ++framesEncoded;
        bytesWritten += size;
        QPID_LOG(debug, "Wrote frame: lastEof=" << lastEof << ", i=" << i);
    }
    frames.erase(frames.begin(), frames.begin()+lastEof);
    lastEof = 0;
    if (bounds) bounds->reduce(bytesWritten);
    if (encode.getPosition() > 0) writeOne(l);
}

void Connector::readbuff(AsynchIO& aio, AsynchIO::BufferBase* buff) {
    framing::Buffer in(buff->bytes+buff->dataStart, buff->dataCount);

    if (!initiated) {
        framing::ProtocolInitiation protocolInit;
        if (protocolInit.decode(in)) {
            //TODO: check the version is correct
            QPID_LOG(debug, "RECV " << identifier << " INIT(" << protocolInit << ")");
        }
        initiated = true;
    }
    AMQFrame frame;
    while(frame.decode(in)){
        QPID_LOG(trace, "RECV " << identifier << ": " << frame);
        input->received(frame);
    }
    // TODO: unreading needs to go away, and when we can cope
    // with multiple sub-buffers in the general buffer scheme, it will
    if (in.available() != 0) {
        // Adjust buffer for used bytes and then "unread them"
        buff->dataStart += buff->dataCount-in.available();
        buff->dataCount = in.available();
        aio.unread(buff);
    } else {
        // Give whole buffer back to aio subsystem
        aio.queueReadBuffer(buff);
    }
}

void Connector::writebuff(AsynchIO& aio_) {
    writer.write(aio_);
}

void Connector::writeDataBlock(const AMQDataBlock& data) {
    AsynchIO::BufferBase* buff = new Buff(maxFrameSize);
    framing::Buffer out(buff->bytes, buff->byteCount);
    data.encode(out);
    buff->dataCount = data.size();
    aio->queueWrite(buff);
}

void Connector::eof(AsynchIO&) {
    handleClosed();
}

// TODO: astitcher 20070908 This version of the code can never time out, so the idle processing
// will never be called
void Connector::run(){
    // Keep the connection impl in memory until run() completes.
    boost::shared_ptr<ConnectionImpl> protect = impl->shared_from_this();
    assert(protect);
    try {
        Dispatcher d(poller);
	
        for (int i = 0; i < 32; i++) {
            aio->queueReadBuffer(new Buff(maxFrameSize));
        }
	
        aio->start(poller);
        d.run();
        aio->queueForDeletion();
        socket.close();
    } catch (const std::exception& e) {
        QPID_LOG(error, e.what());
        handleClosed();
    }
}


}} // namespace qpid::client
