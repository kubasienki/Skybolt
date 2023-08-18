/* Copyright 2012-2020 Matthew Reid
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "QtPropertyReflection.h"
#include "PropertyMetadata.h"
#include "Sprocket/QtTypeConversions.h"
#include <SkyboltCommon/Math/MathUtility.h>
#include <SkyboltEngine/VisObjectsComponent.h>
#include <SkyboltSim/Reflection.h>
#include <SkyboltCommon/Units.h>

#include <QVector3D>

using namespace skybolt;

template <typename T>
std::optional<T> simToDisplayUnitsMultiplier(const rttr::property& property, T v)
{
	auto variant = property.get_metadata(sim::PropertyMetadataType::Units);
	if (variant.is_type<Units>())
	{
		if (variant.get_value<Units>() == Units::Radians)
		{
			return (T)skybolt::math::radToDegD();
		}
	}
	return std::nullopt;
}

template <typename T>
T simUnitToDisplay(const rttr::property& property, T v)
{
	if (auto m = simToDisplayUnitsMultiplier(property, v); m)
	{
		return v * m.value();
	}
	return v;
}

template <typename T>
T displayUnitToSim(const rttr::property& property, T v)
{
	if (auto m = simToDisplayUnitsMultiplier(property, v); m)
	{
		return v / m.value();
	}
	return v;
}

template <typename SimValueT>
QVariant simValueToQt(const rttr::property& property, const SimValueT& value)
{
	return value;
}

template <typename SimValueT>
SimValueT qtValueToSim(const rttr::property& property, const QVariant& value)
{
	return value.value<SimValueT>();
}

template <>
QVariant simValueToQt(const rttr::property& property, const std::string& value)
{
	return QString::fromStdString(value);
}

template <>
std::string qtValueToSim(const rttr::property& property, const QVariant& value)
{
	return value.toString().toStdString();
}

template <>
QVariant simValueToQt(const rttr::property& property, const float& value)
{
	return simUnitToDisplay(property, value);
}

template <>
float qtValueToSim(const rttr::property& property, const QVariant& value)
{
	return displayUnitToSim(property, value.toFloat());
}

template <>
QVariant simValueToQt(const rttr::property& property, const double& value)
{
	return simUnitToDisplay(property, value);
}

template <>
double qtValueToSim(const rttr::property& property, const QVariant& value)
{
	return displayUnitToSim(property, value.toDouble());
}

template <>
QVariant simValueToQt(const rttr::property& property, const sim::Vector3& value)
{
	return toQVector3D(value);
}

template <>
sim::Vector3 qtValueToSim(const rttr::property& property, const QVariant& value)
{
	return toVector3(value.value<QVector3D>());
}

template <>
QVariant simValueToQt(const rttr::property& property, const sim::Quaternion& value)
{
	glm::dvec3 euler = skybolt::math::eulerFromQuat(value) * skybolt::math::radToDegD();
	return QVector3D(euler.x, euler.y, euler.z);
}

template <>
sim::Quaternion qtValueToSim(const rttr::property& property, const QVariant& value)
{
	sim::Vector3 euler = toVector3(value.value<QVector3D>());
	return skybolt::math::quatFromEuler(euler * skybolt::math::radToDegD());
}

static void addMetadata(QtProperty& qtProperty, const rttr::property& rttrProperty)
{
	// TODO: copy metadata from rttr to QT property automatically, without needing to explicitly all types here?
	if (auto variant = rttrProperty.get_metadata(sim::PropertyMetadataType::AttributeType); variant.is_valid())
	{
		qtProperty.setProperty(PropertyMetadataNames::attributeType, variant.get_value<int>());
	}
}

using PropertyFactory = std::function<QtPropertyUpdaterApplier(const RttrInstanceGetter& instanceGetter, const rttr::property& property)>;

template <typename SimValueT, typename QtValueT>
PropertyFactory createPropertyFactory(const QtValueT& defaultValue)
{
	return [defaultValue] (const RttrInstanceGetter& instanceGetter, const rttr::property& property) {
		QtPropertyUpdaterApplier r;
		
		QString name = QString::fromStdString(property.get_name().to_string());
		r.property = PropertiesModel::createVariantProperty(name, defaultValue);
		addMetadata(*r.property, property);
		
		r.updater = [instanceGetter, property] (QtProperty& qtProperty) {
			rttr::instance instance = instanceGetter();
			if (instance.is_valid())
			{
				auto value = property.get_value(instance).get_value<SimValueT>();
				static_cast<VariantProperty&>(qtProperty).setValue(simValueToQt(property, value));
			}
		};
		r.updater(*r.property);

		if (!property.is_readonly())
		{
			r.applier = [instanceGetter, property] (const QtProperty& qtProperty) {
				rttr::instance instance = instanceGetter();
				if (instance.is_valid())
				{
					auto value = qtValueToSim<SimValueT>(property, static_cast<const VariantProperty&>(qtProperty).value);
					property.set_value(instance, value);
				}
			};
		}

		return r;
	};
}

std::optional<QtPropertyUpdaterApplier> rttrPropertyToQt(const RttrInstanceGetter& instanceGetter, const rttr::property& property)
{
	const auto& type = property.get_type();

	static std::map<rttr::type::type_id, PropertyFactory> typePropertyFactories = {
		{ rttr::type::get<std::string>().get_id(), createPropertyFactory<std::string>("") },
		{ rttr::type::get<bool>().get_id(), createPropertyFactory<bool>(false) },
		{ rttr::type::get<int>().get_id(), createPropertyFactory<int>(0) },
		{ rttr::type::get<float>().get_id(), createPropertyFactory<float>(0.f) },
		{ rttr::type::get<double>().get_id(), createPropertyFactory<double>(0.f) },
		{ rttr::type::get<sim::Vector3>().get_id(), createPropertyFactory<sim::Vector3>(QVector3D(0,0,0)) },
		{ rttr::type::get<sim::Quaternion>().get_id(), createPropertyFactory<sim::Quaternion>(QVector3D(0,0,0)) }
	};

	if (const auto& i = typePropertyFactories.find(type.get_id()); i != typePropertyFactories.end())
	{
		return i->second(instanceGetter, property); 
	}
	return std::nullopt;
}

void addRttrPropertiesToModel(PropertiesModel& model, const rttr::array_range<rttr::property>& properties, const RttrInstanceGetter& instanceGetter)
{
	for (const rttr::property& property : properties)
	{
		std::optional<QtPropertyUpdaterApplier> qtProperty = rttrPropertyToQt(instanceGetter, property);
		if (qtProperty)
		{
			model.addProperty(qtProperty->property, qtProperty->updater, qtProperty->applier);
		}
	}
}