// GS-GENERATED: cf1f21c5
// Generated from a .gss file -- see docs/superpowers/specs/2026-09-11-gss-to-cpp-transpiler-design.md. Hand-editing this file is
// fine; doing so is what "optimizing" it means. A future rebuild from
// the .gss source will ask before overwriting a file whose content no
// longer matches the hash above.
#include <GS.h>

namespace GeneratedScripts {

class Ball
{
public:
	void OnUpdate(GS::Entity entity, GS::Scene& scene, double dt)
		{
			const auto pos = entity.Get<GS::TransformComponent>()->Position;
			double x = pos[0];
			double y = pos[1];
			const auto z = pos[2];
			x += vx * dt;
			y += vy * dt;
			if (x - ballRadius < -worldHalfWidth)
			{
				x = -worldHalfWidth + ballRadius;
				vx = std::abs(vx);
			}
			else
			{
				if (x + ballRadius > worldHalfWidth)
				{
					x = worldHalfWidth - ballRadius;
					vx = -std::abs(vx);
				}
			}
			if (y + ballRadius > worldHalfHeight)
			{
				y = worldHalfHeight - ballRadius;
				vy = -std::abs(vy);
			}
			if (y - ballRadius < -worldHalfHeight)
			{
				y = -worldHalfHeight + ballRadius;
				vy = std::abs(vy);
			}
			const auto paddle = ([&]() -> GS::Entity { for (GS::EntityId id : scene.GetEntities()) { auto* tag = scene.GetComponent<GS::TagComponent>(id); if (tag && tag->Name == ("Paddle")) return scene.Wrap(id); } return GS::Entity(); }());
			const auto paddleX = paddle ? paddle.Get<GS::TransformComponent>()->Position[0] : 0;
			const auto overlapsX = std::abs(x - paddleX) * 2 < (ballRadius * 2 + paddleFullWidth);
			const auto overlapsY = std::abs(y - paddleY) * 2 < (ballRadius * 2 + paddleFullHeight);
			if (vy < 0 && overlapsX && overlapsY)
			{
				vy = std::abs(vy);
				const auto offset = (x - paddleX) / (paddleFullWidth * 0.5);
				vx = offset * ballStartSpeed;
			}
			entity.Get<GS::TransformComponent>()->Position = glm::vec3(x, y, z);
		}

private:
	double worldHalfWidth = 1.6;
	double worldHalfHeight = 0.9;
	double ballRadius = 0.028;
	double ballStartSpeed = 1.1;
	double paddleY = -0.75;
	double paddleFullWidth = 0.36;
	double paddleFullHeight = 0.045;
	double vx = ballStartSpeed * 0.45;
	double vy = ballStartSpeed;
};

}
