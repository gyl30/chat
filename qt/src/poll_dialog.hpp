#pragma once

#include <QStringList>
#include <optional>
#include <utility>

class QWidget;
class client_bridge;
class chat_widget;

std::optional<std::pair<QString, QStringList>> compose_poll(QWidget* parent);
void show_poll(client_bridge& client, chat_widget& chat, qint64 room, qint64 message, QWidget* parent);
