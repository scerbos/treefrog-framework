/* Copyright (c) 2010-2013, AOYAMA Kazuharu
 * All rights reserved.
 *
 * This software may be used and distributed according to the terms of
 * the New BSD License, which is incorporated herein by reference.
 */

#include <QCoreApplication>
#include <QEventLoop>
#include <QAtomicInt>
#include <TActionThread>
#include <TWebApplication>
#include <THttpRequest>
#include <TSession>
#include <TApplicationServerBase>
#ifndef Q_CC_MSVC
# include <unistd.h>
#endif
#include "thttpsocket.h"
#include "twebsocket.h"
#include "tsystemglobal.h"
#include "tsessionmanager.h"

static QAtomicInt threadCounter;

int TActionThread::threadCount()
{
#if QT_VERSION >= 0x050000
    return threadCounter.load();
#else
    return (int)threadCounter;
#endif
}


bool TActionThread::waitForAllDone(int msec)
{
    int cnt;
    QTime time;
    time.start();

    while ((cnt = threadCount()) > 0) {
        if (time.elapsed() > msec) {
            break;
        }

        Tf::msleep(10);
        qApp->processEvents();
    }
    return cnt == 0;
}

/*!
  \class TActionThread
  \brief The TActionThread class provides a thread context.
*/

TActionThread::TActionThread(int socket)
    : QThread(), TActionContext(), httpSocket(nullptr)
{
    threadCounter.fetchAndAddOrdered(1);
    TActionContext::socketDesc = socket;
}


TActionThread::~TActionThread()
{
    if (httpSocket)
        delete httpSocket;

    if (TActionContext::socketDesc > 0)
        TF_CLOSE(TActionContext::socketDesc);

    threadCounter.fetchAndAddOrdered(-1);
}


void TActionThread::run()
{
    QList<THttpRequest> reqs;
    QEventLoop eventLoop;
    httpSocket = new THttpSocket;

    if (Q_UNLIKELY(!httpSocket->setSocketDescriptor(TActionContext::socketDesc))) {
        emitError(httpSocket->error());
        goto socket_error;
    }
    TActionContext::socketDesc = 0;

    for (;;) {
        reqs = readRequest(httpSocket);
        tSystemDebug("HTTP request count: %d", reqs.count());

        if (reqs.isEmpty())
            break;

        for (auto &req : reqs) {
            // WebSocket?
            QByteArray connectionHeader = req.header().rawHeader("Connection").toLower();
            if (connectionHeader.contains("upgrade")) {
                QByteArray upgradeHeader = req.header().rawHeader("Upgrade").toLower();
                tSystemDebug("Upgrade: %s", upgradeHeader.data());
                if (upgradeHeader == "websocket") {
                    // Switch to WebSocket
                    if (!handshakeForWebSocket(req.header())) {
                        goto socket_error;
                    }
                }

                goto socket_cleanup;
            }

            TActionContext::execute(req);

            httpSocket->flush();  // Flush socket
            TActionContext::release();
        }

        if (!httpSocket->waitForReadyRead(5000))
            break;
    }

    closeHttpSocket();  // disconnect

socket_cleanup:
    // For cleanup
    while (eventLoop.processEvents()) {}

socket_error:
    delete httpSocket;
    httpSocket = nullptr;
}


void TActionThread::emitError(int socketError)
{
    emit error(socketError);
}


QList<THttpRequest> TActionThread::readRequest(THttpSocket *socket)
{
    QList<THttpRequest> reqs;
    while (!socket->canReadRequest()) {
        // Check idle timeout
        if (Q_UNLIKELY(socket->idleTime() >= 10)) {
            tSystemWarn("Reading a socket timed out after 10 seconds. Descriptor:%d", (int)socket->socketDescriptor());
            break;
        }

        if (Q_UNLIKELY(socket->socketDescriptor() <= 0)) {
            tSystemWarn("Invalid descriptor (disconnected) : %d", (int)socket->socketDescriptor());
            break;
        }

        socket->waitForReadyRead(10);
    }

    if (Q_UNLIKELY(!socket->canReadRequest())) {
        socket->abort();
    } else {
        reqs = socket->read();
    }

    return reqs;
}


qint64 TActionThread::writeResponse(THttpResponseHeader &header, QIODevice *body)
{
    header.setRawHeader("Connection", "Keep-Alive");
    return httpSocket->write(static_cast<THttpHeader*>(&header), body);
}


void TActionThread::closeHttpSocket()
{
    httpSocket->close();
}


bool TActionThread::handshakeForWebSocket(const THttpRequestHeader &header)
{
    if (!TWebSocket::searchEndpoint(header)) {
        return false;
    }

    QByteArray resp = TAbstractWebSocket::handshakeResponse(header).toByteArray();
    httpSocket->writeRawData(resp.data(), resp.length());
    httpSocket->waitForBytesWritten();

    // Switch to WebSocket
    int sd = TApplicationServerBase::duplicateSocket(httpSocket->socketDescriptor());
    TWebSocket *ws = new TWebSocket(sd, httpSocket->peerAddress(), header);
    connect(ws, SIGNAL(disconnected()), ws, SLOT(deleteLater()));
    ws->moveToThread(Tf::app()->thread());

    // WebSocket opening
    TSession session;
    QByteArray sessionId = header.cookie(TSession::sessionName());
    if (!sessionId.isEmpty()) {
        // Finds a session
        session = TSessionManager::instance().findSession(sessionId);
    }
    ws->startWorkerForOpening(session);

    return true;
}
