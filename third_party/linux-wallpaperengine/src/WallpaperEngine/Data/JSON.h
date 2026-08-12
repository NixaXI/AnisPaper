#pragma once

#include "Builders/ColorBuilder.h"

#include <glm/detail/qualifier.hpp>
#include <glm/detail/type_vec1.hpp>
#include <nlohmann/json.hpp>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include "WallpaperEngine/Data/Builders/UserSettingBuilder.h"
#include "WallpaperEngine/Data/Builders/VectorBuilder.h"
#include "WallpaperEngine/Data/Model/Types.h"
#include "WallpaperEngine/Data/Utils/SFINAE.h"
#include "WallpaperEngine/Logging/Log.h"

namespace WallpaperEngine::Data::JSON {
using namespace WallpaperEngine::Data::Builders;
using namespace WallpaperEngine::Data::Model;
using namespace WallpaperEngine::Data::Utils::SFINAE;

class JsonExtensions;

using JSON = nlohmann::basic_json<
    std::map, std::vector, std::string, bool, std::int64_t, std::uint64_t, double, std::allocator,
    nlohmann::adl_serializer, std::vector<std::uint8_t>, JsonExtensions>;

/**
 * Small extensions class that is used as base class of nlohmann's implementation.
 *
 * Provides shorthand methods to reduce the amount of handling required for specific situations
 * (mainly throwing readable exceptions when a value is missing, optional/default values, user settings...)
 */
class JsonExtensions {
public:
    using base_type = JSON;

    template <typename T, typename std::enable_if_t<is_glm_vec<T>::value, int> = 0> [[nodiscard]] T get () const {
	constexpr int length = GlmVecTraits<T>::length;
	constexpr glm::qualifier qualifier = GlmVecTraits<T>::qualifier;

	// call the specialized version of the function
	return get<length, typename GlmVecTraits<T>::type, qualifier> ();
    }
    template <int length, typename type, glm::qualifier qualifier>
    [[nodiscard]] glm::vec<length, type, qualifier> get () const {
	return VectorBuilder::parse<length, type, qualifier> (this->base ().get<std::string> ());
    }
    [[nodiscard]] Model::Color get () const { return ColorBuilder::parse (this->base ().get<std::string> ()); }
    [[nodiscard]] base_type require (const std::string& key, const std::string& message) const {
	auto base = this->base ();
	const auto it = base.find (key);

	if (it == base.end ()) {
	    sLog.exception (message, ". Contents: ", base.dump ());
	}

	return *it;
    }
    template <typename T> [[nodiscard]] T require (const std::string& key, const std::string& message) const {
	auto base = this->base ();
	const auto it = base.find (key);

	if (it == base.end ()) {
	    sLog.exception (message, ". Contents: ", base.dump ());
	}

	return valueAs<T> (*it);
    }
    [[nodiscard]] std::optional<base_type> optional (const std::string& key) const noexcept {
	auto base = this->base ();
	const auto it = base.find (key);
	auto result = std::optional<base_type> {};

	if (it != base.end () && !it->is_null ()) {
	    result.emplace (*it);
	}

	return result;
    }
    template <typename T> [[nodiscard]] std::optional<T> optional (const std::string& key) const {
	auto base = this->base ();
	const auto it = base.find (key);

	if (it == base.end () || it->is_null ()) {
	    return std::nullopt;
	}

	return valueAs<T> (*it);
    }
    template <typename T> [[nodiscard]] T optional (const std::string& key, T defaultValue) const {
	auto base = this->base ();
	const auto it = base.find (key);

	if (it == base.end () || it->is_null ()) {
	    return defaultValue;
	}

	return valueAs<T> (*it);
    }
    [[nodiscard]] UserSettingUniquePtr user (const std::string& key, const Properties& properties) const;
    template <typename T>
    [[nodiscard]] UserSettingUniquePtr
    user (const std::string& key, const Properties& properties, T defaultValue) const {
	static_assert (std::is_same_v<T, Color> == false, "Use color() for color properties");
	const auto value = this->optional (key);

	if (!value.has_value ()) {
	    return UserSettingBuilder::fromValue<T> (defaultValue);
	}

	// performs a second lookup, but handles the actual call to UserSettingParser outside of this header
	// this resolving the include loop
	return this->user (key, properties);
    }
    [[nodiscard]] UserSettingUniquePtr color (const std::string& key, const Properties& properties) const;
    [[nodiscard]] UserSettingUniquePtr
    color (const std::string& key, const Properties& properties, Color defaultValue) const {
	const auto value = this->optional (key);

	if (!value.has_value ()) {
	    return UserSettingBuilder::fromValue<Color> (defaultValue);
	}

	// performs a second lookup, but handles the actual call to UserSettingParser outside of this header
	// this resolving the include loop
	return this->color (key, properties);
    }

    template <int length, typename type, glm::qualifier qualifier> operator glm::vec<length, type, qualifier> () const {
	return get<length, type, qualifier> ();
    }
    template <typename T, typename std::enable_if_t<is_glm_vec<T>::value> = 0> operator T () const {
	constexpr int length = GlmVecTraits<T>::length;
	constexpr glm::qualifier qualifier = GlmVecTraits<T>::qualifier;

	// call the specialized version of the function
	return operator glm::vec<length, typename GlmVecTraits<T>::type, qualifier> ();
    }

private:
    // Workshop projects in the wild sometimes serialize scalar numbers as
    // JSON strings (for example particle rate/control-point fields).  The
    // old implicit nlohmann conversion throws type_error.302; because several
    // optional() overloads were noexcept, that immediately became SIGABRT.
    // Accept only a complete, finite numeric string.  Any malformed value
    // still follows the existing exception path and is rejected by the child.
    template <typename T> [[nodiscard]] static T numericString (const std::string& text) {
	static_assert (std::is_arithmetic_v<T> && !std::is_same_v<T, bool>);
	if constexpr (std::is_integral_v<T>) {
	    T result {};
	    const auto parsed = std::from_chars (text.data (), text.data () + text.size (), result);
	    if (parsed.ec == std::errc {} && parsed.ptr == text.data () + text.size ()) {
		return result;
	    }
	} else {
	    errno = 0;
	    char* tail = nullptr;
	    const double parsed = std::strtod (text.c_str (), &tail);
	    if (errno != ERANGE && tail != text.c_str () && tail == text.c_str () + text.size () && std::isfinite (parsed) &&
		parsed >= -std::numeric_limits<T>::max () && parsed <= std::numeric_limits<T>::max ()) {
		return static_cast<T> (parsed);
	    }
	}
	throw std::invalid_argument ("invalid numeric JSON string: " + text);
    }

    template <typename T> [[nodiscard]] static T valueAs (const base_type& value) {
	if constexpr (std::is_arithmetic_v<T> && !std::is_same_v<T, bool>) {
	    if (value.is_string ()) {
		return numericString<T> (value.get<std::string> ());
	    }
	}
	return value;
    }

    /**
     * @return The base json object to be used by the extension methods
     */
    [[nodiscard]] const base_type& base () const { return *static_cast<const base_type*> (this); }
};

} // namespace WallpaperEngine::Data::JSON
