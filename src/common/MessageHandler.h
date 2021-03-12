/***************************************************************************
* Copyright (c) 2021 Pier Luigi Fiorini <pierluigi.fiorini@gmail.com>
* Copyright (c) 2013 Abdurrahman AVCI <abdurrahmanavci@gmail.com>
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the
* Free Software Foundation, Inc.,
* 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
***************************************************************************/

#ifndef SDDM_MESSAGEHANDLER_H
#define SDDM_MESSAGEHANDLER_H

#include <qlogging.h>

namespace SDDM {

bool isJournalEnabled();

void DaemonMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg);
void HelperMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg);
void GreeterMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg);

bool setupJournalFds(const QString &identifier);

} // namespace SDDM

#endif // SDDM_MESSAGEHANDLER_H
