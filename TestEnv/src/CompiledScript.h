#pragma once

// The uniform interface every compiled (graduated) GSS script's generated
// class satisfies -- see docs/superpowers/specs/
// 2026-09-11-gss-to-cpp-transpiler-design.md's "Generated shape": every
// GeneratedScripts::<Name> class TranspileToCpp emits has the same
// OnStart(GS::Entity, GS::Scene&) / OnUpdate(GS::Entity, GS::Scene&, double)
// shape, so one type-erased entry describes all of them without a virtual
// base class reaching into hand-generated (or hand-optimized) code.

#include <GS.h>

#include <memory>
#include <type_traits>

// A script with only an OnUpdate (no OnStart) is a real, expected shape --
// the interpreted path already tolerates a missing OnStart/OnUpdate (see
// PreparedScript's own `typeof OnStart === 'function' ? OnStart : null`),
// and the first script actually graduated (Paddle, once its globalThis
// write had nowhere left to go) turned out to be exactly this case. These
// detect which methods T actually defines so MakeCompiledScriptEntry<T>
// doesn't hard-require both.
template<typename T, typename = void>
struct HasCompiledOnStart : std::false_type {};
template<typename T>
struct HasCompiledOnStart<T, std::void_t<decltype(std::declval<T&>().OnStart(std::declval<GS::Entity>(), std::declval<GS::Scene&>()))>> : std::true_type {};

template<typename T, typename = void>
struct HasCompiledOnUpdate : std::false_type {};
template<typename T>
struct HasCompiledOnUpdate<T, std::void_t<decltype(std::declval<T&>().OnUpdate(std::declval<GS::Entity>(), std::declval<GS::Scene&>(), std::declval<double>()))>> : std::true_type {};

struct CompiledScriptEntry
{
	const char* ClassName;
	std::shared_ptr<void> (*Create)();
	void (*CallOnStart)(void* instance, GS::Entity entity, GS::Scene& scene);
	void (*CallOnUpdate)(void* instance, GS::Entity entity, GS::Scene& scene, double dt);
};

// Builds one entry for T -- a GeneratedScripts::<Name> class, or any
// hand-written type exposing the same methods. Each lambda is
// non-capturing, so it decays to a plain function pointer and
// CompiledScriptEntry stays a flat struct of pointers, the same shape
// DemoRegistry.h's DemoEntry::Create already uses, rather than paying for
// std::function's indirection.
template<typename T>
inline CompiledScriptEntry MakeCompiledScriptEntry(const char* className)
{
	return CompiledScriptEntry{
		className,
		[]() -> std::shared_ptr<void> { return std::make_shared<T>(); },
		[](void* instance, GS::Entity entity, GS::Scene& scene)
		{
			if constexpr (HasCompiledOnStart<T>::value)
				static_cast<T*>(instance)->OnStart(entity, scene);
		},
		[](void* instance, GS::Entity entity, GS::Scene& scene, double dt)
		{
			if constexpr (HasCompiledOnUpdate<T>::value)
				static_cast<T*>(instance)->OnUpdate(entity, scene, dt);
		}
	};
}
