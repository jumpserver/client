#pragma once

#include "replaysource.h"
#include "theme.h"
#include <QDialog>
#include <QFormLayout>
#include <QLocale>
#include <QStringList>
#include <QStyle>

namespace Replay {

inline QString timeLabel(qint64 ms) {
  const auto s = ms / 1000;
  return QString("%1:%2:%3").arg(s / 3600, 2, 10, QLatin1Char('0'))
      .arg(s / 60 % 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'));
}

inline QString localTime(const QDateTime &date) {
  return date.isValid() ? QLocale().toString(date.toLocalTime(), QLocale::ShortFormat) : QString();
}

inline QString kindLabel(Kind kind) {
  if (kind == Kind::Guacamole) return ClientTheme::text("RDP/VNC 协议录像", "RDP/VNC protocol recording");
  if (kind == Kind::Mp4) return QStringLiteral("MP4");
  return ClientTheme::text("终端录像", "Terminal recording");
}

inline QList<QPair<QString, QString>> metadataFields(const RecordingInfo &info) {
  using ClientTheme::text;
  const auto &m = info.metadata;
  return {{text("文件", "File"), info.label}, {text("格式", "Format"), kindLabel(info.kind)},
          {text("资产", "Asset"), m.asset}, {text("用户", "User"), m.user},
          {text("账号", "Account"), m.account}, {text("协议", "Protocol"), m.protocol.toUpper()},
          {text("来源地址", "Remote address"), m.remoteAddress}, {text("开始时间", "Started"), localTime(m.started)},
          {text("结束时间", "Ended"), localTime(m.ended)}, {text("会话 ID", "Session ID"), m.id}};
}

inline QString metadataSummary(const Metadata &meta) {
  QStringList values;
  for (const auto &value : {meta.user, meta.asset, meta.account})
    if (!value.isEmpty()) values << value;
  return values.join(QStringLiteral(" · "));
}

inline QString detailsTooltip(const QList<QPair<QString, QString>> &fields) {
  QStringList lines;
  for (const auto &field : fields)
    if (!field.second.isEmpty()) lines << field.first.toHtmlEscaped() + ": " + field.second.toHtmlEscaped();
  return "<p>" + lines.join("<br>") + "</p>";
}

inline void showDetails(QWidget *anchor, const QList<QPair<QString, QString>> &fields) {
  auto *popup = new QDialog(anchor, Qt::Popup);
  popup->setObjectName("replayDetailsPopup");
  popup->setAttribute(Qt::WA_DeleteOnClose);
  auto *form = new QFormLayout(popup);
  form->setContentsMargins(14, 12, 14, 12);
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  for (const auto &field : fields) {
    if (field.second.isEmpty()) continue;
    auto *label = new QLabel(field.first, popup);
    label->setProperty("role", "caption");
    auto *value = new ClientTheme::ElidedLabel(field.second, popup);
    value->setTextFormat(Qt::PlainText);
    value->setToolTip(detailsTooltip({field}));
    form->addRow(label, value);
  }
  const QRect available = ClientTheme::dialogAvailableGeometry(popup, true);
  popup->setFixedWidth(qMin(414, qMax(260, available.width() - 24)));
  popup->adjustSize();
  QPoint pos = anchor->mapToGlobal(QPoint(0, anchor->height() + 4));
  if (pos.y() + popup->height() > available.bottom())
    pos.setY(anchor->mapToGlobal(QPoint(0, -popup->height() - 4)).y());
  pos.setY(qBound(available.top(), pos.y(), qMax(available.top(), available.bottom() - popup->height())));
  pos.setX(qBound(available.left(), pos.x(), qMax(available.left(), available.right() - popup->width())));
  popup->move(pos);
  popup->show();
}

inline void setDropHighlight(QWidget *zone, bool active) {
  if (zone->property("dragging").toBool() == active) return;
  zone->setProperty("dragging", active);
  zone->style()->unpolish(zone);
  zone->style()->polish(zone);
  zone->update();
}

} // namespace Replay
