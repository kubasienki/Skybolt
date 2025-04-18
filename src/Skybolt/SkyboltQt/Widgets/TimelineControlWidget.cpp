/* Copyright Matthew Reid
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "TimelineControlWidget.h"

#include "Widgets/TimeControlWidget.h"
#include "Widgets/TimelineWidget.h"

#include <SkyboltEngine/TimeSource.h>

#include <QBoxLayout>

using namespace skybolt;

TimelineControlWidget::TimelineControlWidget(const TimelineControlWidgetConfig& config) :
	QWidget(config.parent)
{
	assert(timeSource);

	auto layout = new QVBoxLayout(this);
	setLayout(layout);

	TimelineWidget* timeline = new TimelineWidget;
	layout->addWidget(timeline);
	mTimeControlWidget = new TimeControlWidget;
	layout->addWidget(mTimeControlWidget);
	layout->addStretch();

	timeline->setTimeSource(config.timeSource);
	
	timeline->setTimelineMode(config.timelineMode->get());
	mTimeControlWidget->setTimelineMode(config.timelineMode->get());
	config.timelineMode->valueChanged.connect([timeline, this] (const TimelineMode& oldValue, const TimelineMode& newValue) {
		timeline->setTimelineMode(newValue);
		mTimeControlWidget->setTimelineMode(newValue);
	});

	config.timeSource->timeChanged.connect([=](double time)
	{
		timeline->setBufferedRange(TimeRange(0, time));
	});

	mTimeControlWidget->setTimeSource(config.timeSource);
	mTimeControlWidget->setRequestedTimeRateSource(config.requestedTimeRate);
	mTimeControlWidget->setActualTimeRateSource(config.actualTimeRate);
}