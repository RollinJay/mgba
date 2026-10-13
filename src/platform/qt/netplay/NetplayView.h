/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <QImage>
#include <QWidget>

#include <vector>

namespace QGBA {

/* One GBA picture, scaled by the largest whole number that fits (or shrunk to fit when
 * even the picture's own size does not) and centered, with an optional caption above it
 * (the player's number and name). */
class NetplayView : public QWidget {
	Q_OBJECT

public:
	explicit NetplayView(QWidget* parent = nullptr);

	/* width x height pixels, rows packed, 0xXXBBGGRR */
	void setPicture(const std::vector<uint32_t>& pixels, int width, int height);
	/* The picture size to lay out for before the first picture (240x160 at first). */
	void setVideoSize(int width, int height);
	void setCaption(const QString&);
	void setPlaceholder(const QString&);
	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

protected:
	void paintEvent(QPaintEvent*) override;

private:
	int m_width = 240;
	int m_height = 160;
	QImage m_image;
	QString m_caption;
	QString m_placeholder;
};

}
