// GS-GENERATED: defa7d55
// Generated from a .gss file -- see docs/superpowers/specs/2026-09-11-gss-to-cpp-transpiler-design.md. Hand-editing this file is
// fine; doing so is what "optimizing" it means. A future rebuild from
// the .gss source will ask before overwriting a file whose content no
// longer matches the hash above.
#include <GS.h>

namespace GeneratedScripts {

class Paddle
{
public:
	void OnUpdate(GS::Entity entity, GS::Scene& scene, double dt)
		{
			const auto pos = entity.Get<GS::TransformComponent>()->Position;
			double x = pos[0];
			const auto y = pos[1];
			const auto z = pos[2];
			if (GS::Input::IsKeyPressed(GS_KEY_LEFT) || GS::Input::IsKeyPressed(GS_KEY_A))
			{
				x -= paddleSpeed * dt;
			}
			if (GS::Input::IsKeyPressed(GS_KEY_RIGHT) || GS::Input::IsKeyPressed(GS_KEY_D))
			{
				x += paddleSpeed * dt;
			}
			const auto limit = worldHalfWidth - paddleHalfWidth;
			x = std::max(-limit, std::min(limit, x));
			entity.Get<GS::TransformComponent>()->Position = glm::vec3(x, y, z);
		}

private:
	double paddleSpeed = 2.2;
	double paddleHalfWidth = 0.18;
	double worldHalfWidth = 1.6;
};

}
