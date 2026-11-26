#pragma once

#include <QtGlobal>

class client_bridge;
class chat_widget;

void compose_poll(client_bridge& client, chat_widget& chat, qint64 room);
