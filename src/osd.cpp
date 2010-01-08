#include "osd.h"

#include <QCoreApplication>
#include <QtDebug>

OSD::OSD(QSystemTrayIcon* tray_icon, QObject* parent)
  : QObject(parent),
    tray_icon_(tray_icon),
    timeout_(5000)
{
  Init();
}

void OSD::SongChanged(const Song &song) {
  QString summary(song.PrettyTitle());
  if (!song.artist().isNull())
    summary = QString("%1 - %2").arg(song.artist(), summary);

  QStringList message_parts;
  if (!song.album().isEmpty())
    message_parts << song.album();
  if (song.disc() > 0)
    message_parts << QString("disc %1").arg(song.disc());
  if (song.track() > 0)
    message_parts << QString("track %1").arg(song.track());

  ShowMessage(summary, message_parts.join(", "), "notification-audio-play");
}

void OSD::Paused() {
  ShowMessage(QCoreApplication::applicationName(), "Paused");
}

void OSD::Stopped() {
  ShowMessage(QCoreApplication::applicationName(), "Playlist finished");
}

void OSD::VolumeChanged(int value) {
  ShowMessage(QCoreApplication::applicationName(), QString("Volume %1%").arg(value));
}
