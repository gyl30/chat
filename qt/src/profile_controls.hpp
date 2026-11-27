#pragma once

#include <QString>

class QToolButton;
class QPushButton;
class QWidget;

QToolButton* profile_action(QWidget* parent, QString const& text, QStringView icon, bool danger = false);
QPushButton* profile_entry(QWidget* parent, QString const& text, QStringView icon, bool danger = false);
void display_notification_action(QToolButton& button, bool muted);
