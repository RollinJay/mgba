/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "NetplayView.h"

#include <QPainter>
#include <QtMath>

using namespace QGBA;

namespace {

const int CAPTION = 22;

}

NetplayView::NetplayView(QWidget* parent)
	: QWidget(parent) {
	setAttribute(Qt::WA_OpaquePaintEvent);
}

void NetplayView::setPicture(const std::vector<uint32_t>& pixels, int width, int height) {
	if (width <= 0 || height <= 0 || pixels.size() != size_t(width) * size_t(height)) {
		return;
	}
	if (width != m_width || height != m_height) {
		setVideoSize(width, height);
	}
	// mGBA's 32-bit pixels are 0xXXBBGGRR, which is RGBX8888 in byte order on little-endian hosts
	m_image = QImage(reinterpret_cast<const uchar*>(pixels.data()), width, height, width * 4, QImage::Format_RGBX8888).copy();
	m_placeholder.clear();
	update();
}

void NetplayView::setVideoSize(int width, int height) {
	m_width = width;
	m_height = height;
	updateGeometry();
	update();
}

void NetplayView::setCaption(const QString& caption) {
	m_caption = caption;
	update();
}

void NetplayView::setPlaceholder(const QString& text) {
	m_placeholder = text;
	m_image = QImage();
	update();
}

QSize NetplayView::sizeHint() const {
	return QSize(m_width * 3, m_height * 3 + (m_caption.isEmpty() ? 0 : CAPTION));
}

QSize NetplayView::minimumSizeHint() const {
	return QSize(m_width / 2, m_height / 2 + (m_caption.isEmpty() ? 0 : CAPTION));
}

void NetplayView::paintEvent(QPaintEvent*) {
	QPainter painter(this);
	painter.fillRect(rect(), QColor(0x14, 0x11, 0x1d));
	QRect area = rect();
	if (!m_caption.isEmpty()) {
		QRect captionRect(area.left() + 6, area.top(), area.width() - 12, CAPTION);
		painter.setPen(QColor(0xe8, 0xe4, 0xf0));
		QFont font = painter.font();
		font.setBold(true);
		painter.setFont(font);
		painter.drawText(captionRect, Qt::AlignLeft | Qt::AlignVCenter, m_caption);
		area.setTop(area.top() + CAPTION);
	}
	// a whole-number scale keeps the pixels square and sharp; below the picture's own size
	// it shrinks to fit, smoothly
	qreal fit = qMin(qreal(area.width()) / m_width, qreal(area.height()) / m_height);
	qreal scale = fit >= 1 ? qFloor(fit) : fit;
	QRect target(0, 0, qRound(m_width * scale), qRound(m_height * scale));
	target.moveCenter(area.center());
	painter.setRenderHint(QPainter::SmoothPixmapTransform, scale < 1);
	if (m_image.isNull()) {
		painter.fillRect(target, QColor(0x2a, 0x24, 0x38));
		painter.setPen(QColor(0x9a, 0x92, 0xb0));
		painter.drawText(target, Qt::AlignCenter | Qt::TextWordWrap, m_placeholder);
		return;
	}
	painter.drawImage(target, m_image);
}
