/******************************************************************************
**
** This file is part of libcommhistory.
**
** Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies).
** Contact: Alexander Shalamov <alexander.shalamov@nokia.com>
**
** This library is free software; you can redistribute it and/or modify it
** under the terms of the GNU Lesser General Public License version 2.1 as
** published by the Free Software Foundation.
**
** This library is distributed in the hope that it will be useful, but
** WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
** or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
** License for more details.
**
** You should have received a copy of the GNU Lesser General Public License
** along with this library; if not, write to the Free Software Foundation, Inc.,
** 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
**
******************************************************************************/

#include <QtDBus/QtDBus>
#include <QDebug>

#include "trackerio.h"
#include "queryrunner.h"
#include "eventmodel.h"
#include "eventmodel_p.h"
#include "adaptor.h"
#include "event.h"
#include "eventtreeitem.h"
#include "constants.h"
#include "commonutils.h"
#include "contactlistener.h"
#include "committingtransaction.h"
#include "eventsquery.h"

using namespace CommHistory;

namespace {
    static const int defaultChunkSize = 50;
}

uint EventModelPrivate::modelSerial = 0;

EventModelPrivate::EventModelPrivate(EventModel *model)
        : isInTreeMode(false)
        , queryMode(EventModel::AsyncQuery)
        , chunkSize(defaultChunkSize)
        , firstChunkSize(0)
        , queryLimit(0)
        , queryOffset(0)
        , isReady(true)
        , messagePartsReady(true)
        , threadCanFetchMore(false)
        , syncOnCommit(false)
        , contactChangesEnabled(false)
        , queryRunner(0)
        , partQueryRunner(0)
        , propertyMask(Event::allProperties())
        , bgThread(0)
        , m_pTracker(0)
{
    q_ptr = model;
    qRegisterMetaType<QList<CommHistory::Event> >();
    new Adaptor(this);
    if (!QDBusConnection::sessionBus().registerObject(
            newObjectPath(), this)) {
        qWarning() << __FUNCTION__ << ": error registering object";
    }
    QDBusConnection::sessionBus().connect(
        QString(), QString(), COMM_HISTORY_SERVICE_NAME, EVENTS_ADDED_SIGNAL,
        this, SLOT(eventsAddedSlot(const QList<CommHistory::Event> &)));
    QDBusConnection::sessionBus().connect(
        QString(), QString(), COMM_HISTORY_SERVICE_NAME, EVENTS_UPDATED_SIGNAL,
        this, SLOT(eventsUpdatedSlot(const QList<CommHistory::Event> &)));
    QDBusConnection::sessionBus().connect(
        QString(), QString(), COMM_HISTORY_SERVICE_NAME, EVENT_DELETED_SIGNAL,
        this, SLOT(eventDeletedSlot(int)));

    resetQueryRunners();
    eventRootItem = new EventTreeItem(Event());
}

EventModelPrivate::~EventModelPrivate()
{
    qDebug() << Q_FUNC_INFO;

    deleteQueryRunners();
    delete eventRootItem;
}


void EventModelPrivate::resetQueryRunners()
{
    deleteQueryRunners();

    queryRunner = new QueryRunner(tracker());
    partQueryRunner = new QueryRunner(tracker());

    connect(queryRunner, SIGNAL(eventsReceived(int, int, QList<CommHistory::Event>)),
            this, SLOT(eventsReceivedSlot(int, int, QList<CommHistory::Event>)));
    connect(queryRunner, SIGNAL(canFetchMoreChanged(bool)),
            this, SLOT(canFetchMoreChangedSlot(bool)));
    connect(queryRunner, SIGNAL(modelUpdated(bool)), this, SLOT(modelUpdatedSlot(bool)));

    connect(partQueryRunner, SIGNAL(messagePartsReceived(int, QList<CommHistory::MessagePart>)),
            this, SLOT(messagePartsReceivedSlot(int, QList<CommHistory::MessagePart>)));
    connect(partQueryRunner, SIGNAL(modelUpdated(bool)), this, SLOT(partsUpdatedSlot(bool)));
    partQueryRunner->enableQueue(true);

    if (bgThread) {
        qDebug() << Q_FUNC_INFO << "MOVE" << queryRunner
                 << partQueryRunner;

        queryRunner->moveToThread(bgThread);
        partQueryRunner->moveToThread(bgThread);
    }
}

void EventModelPrivate::deleteQueryRunners()
{
    if (queryRunner) {
        queryRunner->disconnect(this);
        queryRunner->deleteLater();
        queryRunner = 0;
    }

    if (partQueryRunner) {
        partQueryRunner->disconnect(this);
        partQueryRunner->deleteLater();
        partQueryRunner = 0;
    }
}

bool EventModelPrivate::acceptsEvent(const Event &event) const
{
    Q_UNUSED(event);
    return false;
}

QModelIndex EventModelPrivate::findEventRecursive(int id, EventTreeItem *parent) const
{
    Q_Q(const EventModel);

    for (int row = 0; row < parent->childCount(); row++) {
        if (parent->eventAt(row).id() == id) {
            return q->createIndex(row, 0, parent->child(row));
        } else if (parent->child(row)->childCount()) {
            return findEventRecursive(id, parent->child(row));
        }
    }
    return QModelIndex();
}

QModelIndex EventModelPrivate::findEvent(int id) const
{
    return findEventRecursive(id, eventRootItem);
}

QModelIndex EventModelPrivate::findParent(const Event &event)
{
    Q_UNUSED(event);
    return QModelIndex();
}

bool EventModelPrivate::executeQuery(EventsQuery &query)
{
    qDebug() << __PRETTY_FUNCTION__;

    isReady = false;
    if (queryMode == EventModel::StreamedAsyncQuery) {
        queryRunner->setStreamedMode(true);
        queryRunner->setChunkSize(chunkSize);
        queryRunner->setFirstChunkSize(firstChunkSize);
    } else {
        //if (queryLimit) query.limit(queryLimit);
        //if (queryOffset) query.offset(queryOffset);
    }
    QString sparqlQuery = query.query();
    queryRunner->runEventsQuery(sparqlQuery, query.eventProperties());
    if (queryMode == EventModel::SyncQuery) {
        QEventLoop loop;
        while (!isReady || !messagePartsReady) {
            loop.processEvents(QEventLoop::WaitForMoreEvents);
        }
    }

    return true;
}

bool EventModelPrivate::fillModel(int start,
                                 int end,
                                 QList<CommHistory::Event> events)
{
    Q_UNUSED(start);
    Q_UNUSED(end);

    Q_Q(EventModel);
    qDebug() << __PRETTY_FUNCTION__ << ": read" << events.count() << "events";

    q->beginInsertRows(QModelIndex(), q->rowCount(), q->rowCount() + events.count() - 1);
    foreach (Event event, events) {
        eventRootItem->appendChild(new EventTreeItem(event, eventRootItem));
    }
    q->endInsertRows();

    return false;
}

QString EventModelPrivate::newObjectPath()
{
    return QString(QLatin1String("/CommHistoryModel")) + QString::number(modelSerial++);
}

void EventModelPrivate::clearEvents()
{
    qDebug() << __PRETTY_FUNCTION__;
    delete eventRootItem;
    eventRootItem = new EventTreeItem(Event());
}

void EventModelPrivate::addToModel(Event &event)
{
    Q_Q(EventModel);
    qDebug() << __PRETTY_FUNCTION__ << event.id();
    qDebug() << __PRETTY_FUNCTION__ << event.toString();

    if (!event.contacts().isEmpty()) {
        foreach (Event::Contact contact, event.contacts())
            contactCache.insert(qMakePair(event.localUid(), event.remoteUid()), contact);
    } else {
        setContactFromCache(event);
    }

    QModelIndex index = findParent(event);
    if (index.isValid()) {
        q->beginInsertRows(index, index.row(), index.row());
    } else {
        q->beginInsertRows(QModelIndex(), 0, 0);
    }
    EventTreeItem *item = static_cast<EventTreeItem *>(index.internalPointer());
    if (!item) item = eventRootItem;
    item->prependChild(new EventTreeItem(event, item));
    q->endInsertRows();
}

void EventModelPrivate::modifyInModel(Event &event)
{
    Q_Q(EventModel);
    qDebug() << __PRETTY_FUNCTION__ << event.id();

    if (!event.validProperties().contains(Event::Contacts)) {
        setContactFromCache(event);
    }

    QModelIndex index = findEvent(event.id());
    if (index.isValid()) {
        EventTreeItem *item = static_cast<EventTreeItem *>(index.internalPointer());
        Event oldEvent = item->event();
        oldEvent.copyValidProperties(event);
        item->setEvent(oldEvent);
        QModelIndex bottom = q->createIndex(index.row(),
                                            EventModel::NumberOfColumns - 1,
                                            index.internalPointer());
        emit q->dataChanged(index, bottom);
    }
}

void EventModelPrivate::deleteFromModel(int id)
{
    Q_Q(EventModel);
    qDebug() << __PRETTY_FUNCTION__ << id;
    QModelIndex index = findEvent(id);
    if (index.isValid()) {
        q->beginRemoveRows(index.parent(), index.row(), index.row());
        EventTreeItem *parent = static_cast<EventTreeItem *>(index.parent().internalPointer());
        if (!parent) parent = eventRootItem;
        parent->removeAt(index.row());
        q->endRemoveRows();
    }
}

bool EventModelPrivate::doAddEvent( Event &event )
{
    if (event.type() == Event::UnknownType) {
        qWarning() << Q_FUNC_INFO << "Event type not set";
        return false;
    }

    if (event.direction() == Event::UnknownDirection) {
        qWarning() << Q_FUNC_INFO << "Event direction not set";
        return false;
    }

    if (event.groupId() == -1) {
        if (!event.isDraft() && event.type() != Event::CallEvent) {
            qWarning() << Q_FUNC_INFO << "Group id not set";
            return false;
        }
    }

    if (!tracker()->addEvent(event)) {
        return false;
    }

    return true;
}

bool EventModelPrivate::doDeleteEvent( int id, Event &event )
{
    // fetch event from database
    if (!tracker()->getEvent(id, event)) {
        return false;
    }

    // delete it
    if (!tracker()->deleteEvent(event, bgThread)) {
        return false;
    }
    return true;
}

void EventModelPrivate::eventsReceivedSlot(int start, int end, QList<Event> events)
{
    qDebug() << __PRETTY_FUNCTION__ << ":" << start << end << events.count();

    QMutableListIterator<Event> i(events);
    while (i.hasNext()) {
        Event event = i.next();

        if (findEvent(event.id()).isValid()) {
            end--;
            i.remove();
            continue;
        }

        if (!event.contacts().isEmpty()) {
            foreach (Event::Contact contact, event.contacts())
                contactCache.insert(qMakePair(event.localUid(), event.remoteUid()), contact);
        }

        if (event.type() == Event::MMSEvent && propertyMask.contains(Event::MessageParts)) {
            messagePartsReady = false;
            partQueryRunner->runMessagePartQuery(tracker()->prepareMessagePartQuery(event.url().toString()));
        }
    }

    if (!events.isEmpty()) {
        // now we have some content -> start tracking contacts if enabled
        startContactListening();

        fillModel(start, end, events);
    }

    if (!messagePartsReady)
        partQueryRunner->startQueue();
}

void EventModelPrivate::messagePartsReceivedSlot(int eventId,
                                                 QList<CommHistory::MessagePart> parts)
{
    Q_Q(EventModel);
    qDebug() << __PRETTY_FUNCTION__ << ":" << eventId << parts.count();

    QModelIndex index = findEvent(eventId);
    if (index.isValid()) {
        EventTreeItem *item = static_cast<EventTreeItem *>(index.internalPointer());
        QList<CommHistory::MessagePart> newParts = item->event().messageParts() + parts;
        item->event().setMessageParts(newParts);
        item->event().resetModifiedProperty(Event::MessageParts);
        QModelIndex bottom = q->createIndex(index.row(),
                                            EventModel::NumberOfColumns - 1,
                                            index.internalPointer());
        emit q->dataChanged(index, bottom);
    }
}

void EventModelPrivate::modelUpdatedSlot(bool successful)
{
    qDebug() << __PRETTY_FUNCTION__;

    isReady = true;
    if (successful) {
        if (messagePartsReady)
            emit modelReady(true);
    } else {
        emit modelReady(false);
    }
}

void EventModelPrivate::partsUpdatedSlot(bool successful)
{
    qDebug() << __PRETTY_FUNCTION__;

    messagePartsReady = true;
    if (successful) {
        if (isReady)
            emit modelReady(true);
    } else {
        emit modelReady(false);
    }
}

void EventModelPrivate::eventsAddedSlot(const QList<Event> &events)
{
    qDebug() << __PRETTY_FUNCTION__ << ":" << events.count() << "events";

    foreach (const Event &event, events) {
        QModelIndex index = findEvent(event.id());
        if (index.isValid()) return;

        Event e = event;
        if (acceptsEvent(e))
            addToModel(e);
    }
}

void EventModelPrivate::eventsUpdatedSlot(const QList<Event> &events)
{
    qDebug() << __PRETTY_FUNCTION__ << ":" << events.count();

    foreach (const Event &event, events) {
        QModelIndex index = findEvent(event.id());
        if (!index.isValid()) return;

        Event e = event;
        modifyInModel(e);
    }
}

void EventModelPrivate::eventDeletedSlot(int id)
{
    qDebug() << __PRETTY_FUNCTION__ << ":" << id;

    deleteFromModel(id);
}

CommittingTransaction* EventModelPrivate::commitTransaction(const QList<Event> &events)
{
    CommittingTransaction *t = tracker()->commit();

    if (t) {
        t->addSignal(false,
                    this,
                    "eventsCommitted",
                    Q_ARG(QList<CommHistory::Event>, events),
                    Q_ARG(bool, true));

        t->addSignal(true,
                    this,
                    "eventsCommitted",
                    Q_ARG(QList<CommHistory::Event>, events),
                    Q_ARG(bool, false));
    }

    return t;
}

void EventModelPrivate::canFetchMoreChangedSlot(bool canFetch)
{
    threadCanFetchMore = canFetch;
}

bool EventModelPrivate::canFetchMore() const
{
    return threadCanFetchMore;
}

void EventModelPrivate::changeContactsRecursive(ContactChangeType changeType,
                                                quint32 contactId,
                                                const QString &contactName,
                                                const QList< QPair<QString,QString> > &contactAddresses,
                                                EventTreeItem *parent)
{
    qDebug() << Q_FUNC_INFO;

    Q_Q(EventModel);

    for (int row = 0; row < parent->childCount(); row++) {
        Event *event = &(parent->eventAt(row));

        bool eventChanged = false;

        if (changeType == ContactRemoved) {
            // FIXME: potential risk of QContactLocalId (quint32) not
            // fitting to int. should we change event/group.contactId to
            // uint?
            if ((quint32)event->contactId() == contactId) {
                event->setContactId(0);
                event->setContactName(QString());
                eventChanged = true;
            }
        } else if (changeType == ContactUpdated) {
            if (ContactListener::addressMatchesList(event->localUid(),
                                                    event->remoteUid(),
                                                    contactAddresses)) {

                Event::Contact contact((int)contactId, contactName);

                // If contact is not yet in cache, add it
                QPair<QString, QString> cacheKey = qMakePair(event->localUid(), event->remoteUid());
                if (!contactCache.contains(cacheKey)) {
                    contactCache.insert(cacheKey, contact);
                }

                // TODO: check how this should work with multiple contacts
                if ((quint32)event->contacts().first().first != contactId
                    || event->contacts().first().second != contactName) {
                    event->setContacts(QList<Event::Contact>() << contact);
                    eventChanged = true;
                }
            } else {

                if ((quint32)event->contactId() == contactId) {
                    // event doesn't match the contact anymore, reset contact info
                    event->setContacts(QList<Event::Contact>());
                    eventChanged = true;
                }
            }
        } else {
            qWarning() << "unknown contact change type???";
            break;
        }

        if (eventChanged) {
            QModelIndex left = q->createIndex(row, 0, parent->child(row));
            QModelIndex right = q->createIndex(row, EventModel::NumberOfColumns - 1,
                                               parent->child(row));
            emit q->dataChanged(left, right);
        }

        // dig down to children
        if (parent->child(row)->childCount()) {
            changeContactsRecursive(changeType, contactId, contactName,
                                    contactAddresses, parent->child(row));
        }
    }
}

void EventModelPrivate::slotContactUpdated(quint32 localId,
                                           const QString &contactName,
                                           const QList< QPair<QString,QString> > &contactAddresses)
{
    QMutableMapIterator<QPair<QString,QString>, QPair<int, QString> > i(contactCache);
    while (i.hasNext()) {
        i.next();
        if (ContactListener::addressMatchesList(i.key().first,
                                                i.key().second,
                                                contactAddresses)) {
            // update old record
            i.value().first = localId;
            i.value().second = contactName;
            break;
        } else if ((quint32)(i.value().first) == localId) {
            // old record doesn't match, remove
            i.remove();
            break;
        }
    }

    changeContactsRecursive(ContactUpdated, localId, contactName, contactAddresses, eventRootItem);
}

void EventModelPrivate::slotContactRemoved(quint32 localId)
{
    QMutableMapIterator<QPair<QString,QString>, QPair<int, QString> > i(contactCache);
    while (i.hasNext()) {
        i.next();
        if ((quint32)(i.value().first) == localId)
            i.remove();
    }

    changeContactsRecursive(ContactRemoved, localId, QString(), QList< QPair<QString,QString> >(), eventRootItem);
}

TrackerIO* EventModelPrivate::tracker()
{
    return TrackerIO::instance();
}

bool EventModelPrivate::setContactFromCache(CommHistory::Event &event)
{
    Event::Contact contact = contactCache.value(qMakePair(event.localUid(),
                                                          event.remoteUid()));
    if (contact.first > 0) {
        event.setContacts(QList<Event::Contact>() << contact);
        return true;
    }
    return false;
}

void EventModelPrivate::startContactListening()
{
    if (contactChangesEnabled && !contactListener) {
        contactListener = ContactListener::instance();
        connect(contactListener.data(),
                SIGNAL(contactUpdated(quint32, const QString&, const QList<QPair<QString,QString> >&)),
                this,
                SLOT(slotContactUpdated(quint32, const QString&, const QList<QPair<QString,QString> >&)));
        connect(contactListener.data(),
                SIGNAL(contactRemoved(quint32)),
                this,
                SLOT(slotContactRemoved(quint32)));
    }
}
