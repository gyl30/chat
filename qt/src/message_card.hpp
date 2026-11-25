#pragma once

#include <QFont>
#include <QString>

class QPainter;

// A null painter measures the same logical rectangle that is painted at the origin.
int layout_bundle_card(QPainter* painter, QFont const& font, int width, QString const& summary);
