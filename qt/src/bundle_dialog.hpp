#pragma once

#include <QtGlobal>

class QWidget;
class client_bridge;
class chat_widget;
void show_bundle(client_bridge& client, chat_widget& chat, qint64 room, qint64 message, QWidget* parent);
