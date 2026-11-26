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
class avatar_cache;
enum poll_action
{
    poll_results = -1,
    poll_submit = -2,
    poll_solution = -3
};
QList<int> poll_percentages(chat::poll_summary const& poll);
bool has_poll_results(chat::poll_summary const& poll);
int layout_poll_card(QPainter* painter,
                     QFont const& font,
                     int width,
                     QString const& question,
                     chat::poll_summary const& poll,
                     QPoint const* point = nullptr,
                     int* action = nullptr,
                     bool pending = false,
                     qint64 draft = 0,
                     QString* tooltip = nullptr,
                     avatar_cache const* avatars = nullptr);
