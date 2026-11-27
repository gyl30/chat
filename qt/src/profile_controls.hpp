#pragma once

#include <QString>

class QToolButton;
class QPushButton;
class QWidget;

inline constexpr int profile_avatar_size = 96;
QToolButton* profile_action(QWidget* parent, QString const& text, QStringView icon, bool danger = false);
QPushButton* profile_entry(QWidget* parent, QString const& text, QStringView icon, bool danger = false, bool navigation = false);
void display_notification_action(QToolButton& button, bool muted);
