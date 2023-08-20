/* Copyright 2012-2020 Matthew Reid
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "DefaultViewportMouseEventHandler.h"
#include "Sprocket/SceneSelectionModel.h"
#include "Sprocket/Input/ViewportInputSystem.h"
#include "Sprocket/Widgets/ViewportWidget.h"

#include <SkyboltSim/Components/NameComponent.h>

using namespace skybolt;

DefaultViewportMouseEventHandler::DefaultViewportMouseEventHandler(DefaultViewportMouseEventHandlerConfig config) :
	mViewportWidget(config.viewportWidget),
	mViewportInput(config.viewportInput),
	mEntityObjectRegistry(std::move(config.entityObjectRegistry)),
	mSelectionModel(config.sceneSelectionModel)
{
	assert(mViewportWidget);
	assert(mViewportInput);
	assert(mEntityObjectRegistry);
	assert(mSelectionModel);
}

bool DefaultViewportMouseEventHandler::mousePressed(const QPointF& position, Qt::MouseButton button, const Qt::KeyboardModifiers& modifiers)
{
	if (button == Qt::MouseButton::LeftButton)
	{
		if (mViewportInput)
		{
			mViewportInput->setKeyboardEnabled(true);
			return true;
		}
	}
	return false;
}

bool DefaultViewportMouseEventHandler::mouseReleased(const QPointF& position, Qt::MouseButton button)
{
	if (mViewportInput)
	{
		mViewportInput->setKeyboardEnabled(false);
		mViewportInput->setMouseEnabled(false);
	}

	if (button == Qt::MouseButton::LeftButton)
	{
		sim::EntityId selectedEntityId = sim::nullEntityId();
		auto hasNamePredicate = [] (const skybolt::sim::Entity& e) { return !getName(e).empty(); };
			
		std::optional<PickedSceneObject> object = mViewportWidget->pickSceneObjectAtPointInWindow(position, hasNamePredicate);
		ScenarioObjectPtr scenarioObject = object->entity ? mEntityObjectRegistry->findByName(getName(*object->entity)) : nullptr;
		selectItems({scenarioObject});
		return true;
	}
	return false;
}

bool DefaultViewportMouseEventHandler::mouseMoved(const QPointF& position, Qt::MouseButtons buttons)
{
	if (buttons & Qt::LeftButton)
	{
		if (mViewportInput)
		{
			mViewportInput->setMouseEnabled(true);
			return true;
		}
	}
	return false;
}

void DefaultViewportMouseEventHandler::selectItems(const std::vector<ScenarioObjectPtr>& objects)
{
	mSelectionModel->setSelectedItems(objects);
}