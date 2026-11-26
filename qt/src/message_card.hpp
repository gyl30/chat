#pragma once

#include <QFont>
#include <QString>

class QPainter;

// A null painter measures the same logical rectangle that is painted at the origin.
int layout_bundle_card(QPainter* painter, QFont const& font, int width, QString const& summary);

namespace chat
{
struct poll_summary;
}
int layout_poll_card(QPainter* painter,
                     QFont const& font,
                     int width,
                     QString const& question,
                     chat::poll_summary const& poll,
                     QPoint const* point = nullptr,
                     int* action = nullptr,
                     bool pending = false);
