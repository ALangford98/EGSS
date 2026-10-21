#pragma once

// A recorded human movement, and that movement put onto a Humanoid.
//
// The recordings are the CMU Graphics Lab Motion Capture Database's
// (mocap.cs.cmu.edu: "free for all uses ... you may include this data in
// commercially-sold products, but you may not resell this data directly"),
// in its own two files: an ASF, the skeleton -- each bone's direction and
// length standing in the rest pose, the frame its rotations are measured
// in, which of them it has -- and an AMC, the frames, 120 a second, each a
// list of every bone's rotations in degrees and the root's position.
//
// Why reference at all: every movement the rig makes was written from what
// a person seemed to do, and checked against arithmetic -- never against a
// person. A recording is the person. Played on his own bodies (a ghost
// beside him), it shows where he is wrong; measured against him, it says by
// how much.
//
// Putting it on him (Retarget) takes the recording's rotations and his own
// lengths: each of his bodies hangs from its joint as he was built, turned
// the way the recorded bone is turned. So the ghost has his proportions
// and the recording's movement, and the two can be compared directly.
// His thirteen bodies take the recording's thirty-one bones like this:
// the pelvis the root; the torso the thorax (the three spine bones below it
// fold into his one spine joint); the head the head; thigh, shin, foot,
// upper arm and forearm the femur, tibia, foot, humerus and radius. CMU's
// left bones are his side 1 (+x of his own frame, which is a person's left
// as he faces +z), its right bones his side 0.

#include <GS.h>

#include "Humanoid.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

class MotionClip
{
public:
	// The clips a folder offers, from its clips.txt: a name, then the
	// skeleton and frames files, per line ('#' starts a comment).
	struct Entry { std::string Name, Skeleton, Frames, About; };
	static std::vector<Entry> ReadIndex(const std::string& folder)
	{
		std::vector<Entry> out;
		std::ifstream in(folder + "/clips.txt");
		std::string line;
		while (std::getline(in, line))
		{
			if (line.empty() || line[0] == '#')
				continue;
			std::istringstream words(line);
			Entry e;
			if (!(words >> e.Name >> e.Skeleton >> e.Frames))
				continue;
			std::getline(words, e.About);
			e.About.erase(0, e.About.find_first_not_of(' '));
			out.push_back(e);
		}
		return out;
	}

	struct Bone
	{
		std::string Name;
		glm::vec3 Direction{ 0.0f, 1.0f, 0.0f };   // in the rest pose, the world's frame
		float Length = 0.0f;                       // metres
		glm::quat C{ 1.0f, 0.0f, 0.0f, 0.0f };     // the frame its rotations are measured in
		int Dof[3] = { -1, -1, -1 };               // which axis (0 x, 1 y, 2 z) each value turns about
		int DofCount = 0;
		int Parent = -1;                           // -1: hangs from the root
	};

	// One moment: each bone's rotation in the world and where it starts and
	// ends, in metres, in the recording's own frame (y up; standing, a
	// person faces +z with their left towards +x).
	struct Pose
	{
		glm::vec3 Root{ 0.0f };
		glm::quat RootTurn{ 1.0f, 0.0f, 0.0f, 0.0f };
		std::vector<glm::quat> Turn;
		std::vector<glm::vec3> Start, End;
	};

	bool Load(const std::string& asfPath, const std::string& amcPath, std::string* error = nullptr)
	{
		m_Bones.clear();
		m_Frames.clear();
		if (!LoadSkeleton(asfPath, error) || !LoadFrames(amcPath, error))
			return false;
		return !m_Frames.empty();
	}

	float FrameRate() const { return 120.0f; }
	int Frames() const { return (int)m_Frames.size(); }
	float Duration() const { return m_Frames.empty() ? 0.0f : (float)(m_Frames.size() - 1) / FrameRate(); }
	const std::vector<Bone>& Bones() const { return m_Bones; }

	int BoneIndex(const std::string& name) const
	{
		for (size_t i = 0; i < m_Bones.size(); i++)
			if (m_Bones[i].Name == name)
				return (int)i;
		return -1;
	}

	// The pose at `time` seconds, between the two frames either side of it:
	// each bone's own rotation slerped, the root's position lerped, and the
	// skeleton built from those -- so a bone is never stretched by blending
	// two positions.
	void Evaluate(float time, Pose& out) const
	{
		out.Turn.assign(m_Bones.size(), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
		out.Start.assign(m_Bones.size(), glm::vec3(0.0f));
		out.End.assign(m_Bones.size(), glm::vec3(0.0f));
		if (m_Frames.empty())
			return;
		float f = glm::clamp(time * FrameRate(), 0.0f, (float)(m_Frames.size() - 1));
		size_t a = (size_t)f, b = std::min(a + 1, m_Frames.size() - 1);
		float w = f - (float)a;
		const Frame& A = m_Frames[a];
		const Frame& B = m_Frames[b];
		out.Root = glm::mix(A.Root, B.Root, w);
		out.RootTurn = glm::normalize(glm::slerp(A.RootTurn, B.RootTurn, w));
		// Parents come before their children in the file's hierarchy order,
		// which m_Order keeps.
		for (int i : m_Order)
		{
			const Bone& bone = m_Bones[(size_t)i];
			glm::quat local = glm::normalize(glm::slerp(A.Local[(size_t)i], B.Local[(size_t)i], w));
			glm::quat parentTurn = bone.Parent < 0 ? out.RootTurn : out.Turn[(size_t)bone.Parent];
			glm::vec3 start = bone.Parent < 0 ? out.Root : out.End[(size_t)bone.Parent];
			out.Turn[(size_t)i] = glm::normalize(parentTurn * bone.C * local * glm::conjugate(bone.C));
			out.Start[(size_t)i] = start;
			out.End[(size_t)i] = start + out.Turn[(size_t)i] * (bone.Direction * bone.Length);
		}
	}

private:
	struct Frame
	{
		glm::vec3 Root{ 0.0f };
		glm::quat RootTurn{ 1.0f, 0.0f, 0.0f, 0.0f };
		std::vector<glm::quat> Local;
	};

	// CMU's lengths are in units of 1/0.45 inch (":units length 0.45").
	static constexpr float Metres = 0.0254f / 0.45f;

	// Turns in an ASF's "XYZ" order: about x first, then y, then z.
	static glm::quat Euler(const glm::vec3& degrees)
	{
		return glm::normalize(glm::angleAxis(glm::radians(degrees.z), glm::vec3(0.0f, 0.0f, 1.0f))
			* glm::angleAxis(glm::radians(degrees.y), glm::vec3(0.0f, 1.0f, 0.0f))
			* glm::angleAxis(glm::radians(degrees.x), glm::vec3(1.0f, 0.0f, 0.0f)));
	}

	static std::string Lower(std::string s)
	{
		for (char& c : s)
			c = (char)std::tolower((unsigned char)c);
		return s;
	}

	bool LoadSkeleton(const std::string& path, std::string* error)
	{
		std::ifstream in(path);
		if (!in)
		{
			if (error) *error = "cannot open " + path;
			return false;
		}
		std::string line, section;
		Bone bone;
		bool inBone = false;
		std::map<std::string, std::string> parentOf;
		std::vector<std::string> order;
		while (std::getline(in, line))
		{
			std::istringstream words(line);
			std::string key;
			if (!(words >> key) || key[0] == '#')
				continue;
			if (key[0] == ':')
			{
				section = key;
				continue;
			}
			if (section == ":bonedata")
			{
				if (key == "begin") { bone = Bone{}; inBone = true; }
				else if (key == "end" && inBone) { m_Bones.push_back(bone); inBone = false; }
				else if (key == "name") words >> bone.Name;
				else if (key == "direction") words >> bone.Direction.x >> bone.Direction.y >> bone.Direction.z;
				else if (key == "length") { words >> bone.Length; bone.Length *= Metres; }
				else if (key == "axis")
				{
					glm::vec3 a;
					words >> a.x >> a.y >> a.z;
					bone.C = Euler(a);
				}
				else if (key == "dof")
				{
					std::string d;
					while (words >> d && bone.DofCount < 3)
						bone.Dof[bone.DofCount++] = d == "rx" ? 0 : d == "ry" ? 1 : 2;
				}
			}
			else if (section == ":hierarchy")
			{
				if (key == "begin" || key == "end")
					continue;
				std::string child;
				while (words >> child)
				{
					parentOf[child] = key;
					order.push_back(child);
				}
			}
		}
		for (Bone& b : m_Bones)
		{
			auto it = parentOf.find(b.Name);
			b.Parent = (it == parentOf.end() || it->second == "root") ? -1 : BoneIndex(it->second);
		}
		m_Order.clear();
		for (const std::string& name : order)
			if (int i = BoneIndex(name); i >= 0)
				m_Order.push_back(i);
		return true;
	}

	bool LoadFrames(const std::string& path, std::string* error)
	{
		std::ifstream in(path);
		if (!in)
		{
			if (error) *error = "cannot open " + path;
			return false;
		}
		std::string line;
		Frame* frame = nullptr;
		while (std::getline(in, line))
		{
			if (line.empty() || line[0] == '#' || line[0] == ':')
				continue;
			std::istringstream words(line);
			std::string key;
			if (!(words >> key))
				continue;
			if (std::isdigit((unsigned char)key[0]))
			{
				m_Frames.push_back(Frame{});
				frame = &m_Frames.back();
				frame->Local.assign(m_Bones.size(), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
				continue;
			}
			if (!frame)
				continue;
			if (key == "root")
			{
				glm::vec3 t, r;
				words >> t.x >> t.y >> t.z >> r.x >> r.y >> r.z;
				frame->Root = t * Metres;
				frame->RootTurn = Euler(r);
				continue;
			}
			int i = BoneIndex(key);
			if (i < 0)
				continue;
			const Bone& bone = m_Bones[(size_t)i];
			glm::vec3 r(0.0f);
			for (int k = 0; k < bone.DofCount; k++)
			{
				float v = 0.0f;
				words >> v;
				r[bone.Dof[k]] = v;
			}
			frame->Local[(size_t)i] = Euler(r);
		}
		return true;
	}

	std::vector<Bone> m_Bones;
	std::vector<int> m_Order;
	std::vector<Frame> m_Frames;
};

// A clip on a Humanoid's bodies: where each of them would be, in the order
// of Humanoid::Bodies(), for any moment of the clip.
class ClipRetarget
{
public:
	// Pelvis, torso, head, thigh x2, shin x2, foot x2, upper arm x2, forearm x2.
	static constexpr int BodyCount = 13;

	bool Setup(const MotionClip& clip, const Humanoid& rig)
	{
		m_Clip = &clip;
		const Humanoid::RestSkeleton& R = rig.Rest();
		const auto& build = rig.BuildPose();
		if (build.size() < (size_t)BodyCount)
			return false;
		for (int i = 0; i < BodyCount; i++)
			m_Build[i] = build[(size_t)i];
		m_HipCentre = 0.5f * (R.Hip[0] + R.Hip[1]);
		m_Rest = R;

		auto bone = [&](const char* name) { return clip.BoneIndex(name); };
		auto restVector = [&](std::initializer_list<const char*> names)
		{
			glm::vec3 v(0.0f);
			for (const char* n : names)
				if (int b = bone(n); b >= 0)
					v += clip.Bones()[(size_t)b].Direction * clip.Bones()[(size_t)b].Length;
			return v;
		};
		auto link = [&](int body, const char* turnBone, const glm::vec3& rigAxis, const glm::vec3& clipAxis)
		{
			m_Link[body].Bone = bone(turnBone);
			m_Link[body].Align = Between(glm::normalize(rigAxis), glm::normalize(clipAxis));
		};

		link(1, "thorax", R.Neck - R.Spine, restVector({ "lowerback", "upperback", "thorax" }));
		link(2, "head", build[2].first - R.Neck, restVector({ "lowerneck", "upperneck", "head" }));
		for (int side = 0; side < 2; side++)
		{
			// CMU's left is his side 1.
			std::string p = side == 1 ? "l" : "r";
			glm::vec3 footBottom = build[(size_t)(7 + side)].first + glm::vec3(0.0f, -0.05f, 0.11f) * rig.GetProportions().Scale;
			link(3 + side, (p + "femur").c_str(), R.Knee[side] - R.Hip[side], restVector({ (p + "femur").c_str() }));
			link(5 + side, (p + "tibia").c_str(), R.Ankle[side] - R.Knee[side], restVector({ (p + "tibia").c_str() }));
			link(7 + side, (p + "foot").c_str(), footBottom - R.Ankle[side], restVector({ (p + "foot").c_str() }));
			link(9 + side, (p + "humerus").c_str(), R.Elbow[side] - R.Shoulder[side], restVector({ (p + "humerus").c_str() }));
			glm::vec3 wrist = 2.0f * build[(size_t)(11 + side)].first - R.Elbow[side];
			link(11 + side, (p + "radius").c_str(), wrist - R.Elbow[side], restVector({ (p + "radius").c_str() }));
		}
		for (int i = 1; i < BodyCount; i++)
			if (m_Link[i].Bone < 0)
				return false;

		// His legs over the recording's: how much to shrink or grow its
		// travel so his feet cover the ground its feet did.
		float clipLeg = glm::length(restVector({ "lfemur" })) + glm::length(restVector({ "ltibia" }));
		float rigLeg = glm::length(R.Knee[1] - R.Hip[1]) + glm::length(R.Ankle[1] - R.Knee[1]);
		m_Scale = clipLeg > 1e-4f ? rigLeg / clipLeg : 1.0f;
		m_Ground = 0.0f;
		MeasureGround(rig);
		return true;
	}

	float Scale() const { return m_Scale; }

	// His bodies at `time`, in the recording's frame scaled to him, the
	// lowest a foot comes over the clip on y = 0.
	void Pose(float time, glm::vec3* position, glm::quat* orientation) const
	{
		MotionClip::Pose p;
		m_Clip->Evaluate(time, p);
		PoseFrom(p, position, orientation);
	}

	// The recording's moment as Humanoid::LocoPose holds it: where his hip
	// centre is (scaled to him, its floor at 0) and the turn D of each of his
	// bodies from how it was built -- what PoseFrom places them with. Turns,
	// unlike positions, can be blended without stretching a limb.
	void Turns(const MotionClip::Pose& p, glm::vec3& root, glm::quat* D) const
	{
		root = p.Root * m_Scale;
		root.y -= m_Ground;
		D[0] = p.RootTurn;
		for (int i = 1; i < BodyCount; i++)
			D[i] = glm::normalize(p.Turn[(size_t)m_Link[i].Bone] * m_Link[i].Align);
	}

	void PoseFrom(const MotionClip::Pose& p, glm::vec3* position, glm::quat* orientation) const
	{
		glm::vec3 root = p.Root * m_Scale;
		root.y -= m_Ground;
		// Each body hangs from its joint as built, turned by D (the
		// recorded bone's turn, after the turn that lines his rest bone up
		// with the recording's): centre = joint + D (built centre - built
		// joint), and the next joint down the same way.
		auto place = [&](int body, const glm::quat& D, const glm::vec3& joint, const glm::vec3& restJoint)
		{
			position[body] = joint + D * (m_Build[body].first - restJoint);
			orientation[body] = glm::normalize(D * m_Build[body].second);
		};
		auto turn = [&](int body) { return glm::normalize(p.Turn[(size_t)m_Link[body].Bone] * m_Link[body].Align); };
		auto carry = [&](const glm::quat& D, const glm::vec3& joint, const glm::vec3& restJoint, const glm::vec3& restNext)
		{
			return joint + D * (restNext - restJoint);
		};

		glm::quat pelvis = p.RootTurn;
		place(0, pelvis, root, m_HipCentre);
		glm::vec3 spine = carry(pelvis, root, m_HipCentre, m_Rest.Spine);
		glm::quat torso = turn(1);
		place(1, torso, spine, m_Rest.Spine);
		glm::vec3 neck = carry(torso, spine, m_Rest.Spine, m_Rest.Neck);
		place(2, turn(2), neck, m_Rest.Neck);
		for (int side = 0; side < 2; side++)
		{
			glm::vec3 hip = carry(pelvis, root, m_HipCentre, m_Rest.Hip[side]);
			glm::quat thigh = turn(3 + side);
			place(3 + side, thigh, hip, m_Rest.Hip[side]);
			glm::vec3 knee = carry(thigh, hip, m_Rest.Hip[side], m_Rest.Knee[side]);
			glm::quat shin = turn(5 + side);
			place(5 + side, shin, knee, m_Rest.Knee[side]);
			glm::vec3 ankle = carry(shin, knee, m_Rest.Knee[side], m_Rest.Ankle[side]);
			place(7 + side, turn(7 + side), ankle, m_Rest.Ankle[side]);
			glm::vec3 shoulder = carry(torso, spine, m_Rest.Spine, m_Rest.Shoulder[side]);
			glm::quat upper = turn(9 + side);
			place(9 + side, upper, shoulder, m_Rest.Shoulder[side]);
			glm::vec3 elbow = carry(upper, shoulder, m_Rest.Shoulder[side], m_Rest.Elbow[side]);
			place(11 + side, turn(11 + side), elbow, m_Rest.Elbow[side]);
		}
	}

private:
	struct Link
	{
		int Bone = -1;
		glm::quat Align{ 1.0f, 0.0f, 0.0f, 0.0f };
	};

	// The shortest turn taking `from` to `to`, so a limb's twist is kept:
	// his arm hanging and the recording's held straight out (its rest pose
	// is a T) differ by a turn about the way he faces, and nothing else.
	static glm::quat Between(const glm::vec3& from, const glm::vec3& to)
	{
		float d = glm::clamp(glm::dot(from, to), -1.0f, 1.0f);
		glm::vec3 axis = glm::cross(from, to);
		if (glm::length(axis) < 1e-6f)
		{
			if (d > 0.0f)
				return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			axis = std::fabs(from.x) < 0.9f ? glm::cross(from, glm::vec3(1.0f, 0.0f, 0.0f)) : glm::cross(from, glm::vec3(0.0f, 0.0f, 1.0f));
		}
		return glm::angleAxis(std::acos(d), glm::normalize(axis));
	}

	// Where the floor is under the recording: the lowest the soles come,
	// taken frame by frame, a low percentile of that over the clip (a
	// planted foot), so a clip recorded with its floor anywhere stands on 0.
	void MeasureGround(const Humanoid& rig)
	{
		std::vector<float> lows;
		glm::vec3 position[BodyCount];
		glm::quat orientation[BodyCount];
		float halfY = 0.05f * rig.GetProportions().Scale;
		for (float t = 0.0f; t <= m_Clip->Duration(); t += 1.0f / 30.0f)
		{
			Pose(t, position, orientation);
			float low = 1e9f;
			for (int side = 0; side < 2; side++)
				low = std::min(low, position[7 + side].y - halfY);
			lows.push_back(low);
		}
		if (lows.empty())
			return;
		std::sort(lows.begin(), lows.end());
		m_Ground = lows[lows.size() / 10];
	}

	const MotionClip* m_Clip = nullptr;
	std::pair<glm::vec3, glm::quat> m_Build[BodyCount];
	Humanoid::RestSkeleton m_Rest;
	glm::vec3 m_HipCentre{ 0.0f };
	Link m_Link[BodyCount];
	float m_Scale = 1.0f;
	float m_Ground = 0.0f;
};

// --- Locomotion: the strides his walk, jog and run are posed from ----------
//
// One stride of a recording -- two steps, from his side 1's foot landing to
// its landing again -- chosen where the person goes straightest, nearest the
// middle of the clip, and made into a loop he can play at any speed
// (Runtime/Locomotion.h). Each frame is his hip centre and every body's turn
// in a frame that travels with the stride at its average velocity, facing +z:
// what is left is the stride itself -- the bob, the sway, the legs going
// back under him -- and the controller supplies the travel. The ends of a
// stride never meet exactly (a person does not repeat), so what separates
// them is spread over the stride, a little each frame, rather than left as
// a jump at the seam.
// The lowest corner of either of his feet, posed so. ClipRetarget's floor
// assumes a sole 5 cm under the foot's centre and a foot held flat; his box
// and a recorded foot's tilt put the corners lower than that, by enough
// that every recorded pose had his soles under the ground -- pushed up out
// of it, the knees took up the difference, and he stood with them bent 41
// degrees (5 in the old walk), too crouched to step when shoved.
inline float LowestSole(const Humanoid& rig, const glm::vec3* P, const glm::quat* Q)
{
	const glm::vec3 he = rig.FootHalfExtents();
	float lowest = 1e9f;
	for (int side = 0; side < 2; side++)
		for (float x : { -he.x, he.x })
			for (float z : { -he.z, he.z })
				lowest = std::min(lowest, (P[7 + side] + Q[7 + side] * glm::vec3(x, -he.y, z)).y);
	return lowest;
}

inline bool BakeLocoCycle(const MotionClip& clip, const Humanoid& rig, const std::string& name, LocoCycle& out)
{
	ClipRetarget map;
	if (!map.Setup(clip, rig))
		return false;
	const float rate = clip.FrameRate();
	const int n = clip.Frames();
	if (n < 60)
		return false;
	const glm::vec3 up(0.0f, 1.0f, 0.0f);
	std::vector<glm::vec3> root((size_t)n), foot[2] = { std::vector<glm::vec3>((size_t)n), std::vector<glm::vec3>((size_t)n) };
	std::vector<std::array<glm::quat, LocoPose::Bodies>> turns((size_t)n);
	std::vector<float> facing((size_t)n), sole((size_t)n);
	MotionClip::Pose p;
	glm::vec3 P[ClipRetarget::BodyCount];
	glm::quat Q[ClipRetarget::BodyCount];
	for (int k = 0; k < n; k++)
	{
		clip.Evaluate((float)k / rate, p);
		map.Turns(p, root[(size_t)k], turns[(size_t)k].data());
		map.PoseFrom(p, P, Q);
		foot[0][(size_t)k] = P[7];
		foot[1][(size_t)k] = P[8];
		sole[(size_t)k] = LowestSole(rig, P, Q);
		glm::vec3 f = glm::normalize(glm::slerp(Q[0], Q[1], 0.5f)) * glm::vec3(0.0f, 0.0f, 1.0f);
		float yaw = std::atan2(f.x, f.z);
		if (k > 0)
		{
			float d = yaw - facing[(size_t)k - 1];
			while (d > glm::pi<float>()) d -= glm::two_pi<float>();
			while (d < -glm::pi<float>()) d += glm::two_pi<float>();
			yaw = facing[(size_t)k - 1] + d;
		}
		facing[(size_t)k] = yaw;
	}

	// Each foot down or swinging: down while low and slow, up again once it
	// rises or speeds away. Not a walking rule alone (under 0.35 m/s down,
	// past 0.8 up, as the old set pieces had it): a running foot rolls from
	// heel to toe at more than that while it is planted, and was down for 8%
	// of a stride where a runner's stance is about a third. So the bar rises with the
	// clip's pace, and the foot must be near the floor as well.
	float lowest = 1e9f;
	for (int k = 0; k < n; k++)
		lowest = std::min(lowest, std::min(foot[0][(size_t)k].y, foot[1][(size_t)k].y));
	glm::vec3 overall = root[(size_t)n - 1] - root[0];
	overall.y = 0.0f;
	const float pace = glm::length(overall) / clip.Duration();
	const float scale = rig.GetProportions().Scale;
	const float slow = std::max(0.35f, 0.3f * pace), fast = std::max(0.8f, 0.6f * pace);
	std::vector<uint8_t> down[2] = { std::vector<uint8_t>((size_t)n, 1), std::vector<uint8_t>((size_t)n, 1) };
	for (int side = 0; side < 2; side++)
	{
		bool isDown = true;
		for (int k = 1; k < n; k++)
		{
			float speed = glm::length(foot[side][(size_t)k] - foot[side][(size_t)k - 1]) * rate;
			float height = foot[side][(size_t)k].y - lowest;
			if (isDown && (speed > fast || height > 0.07f * scale)) isDown = false;
			else if (!isDown && speed < slow && height < 0.04f * scale) isDown = true;
			down[side][(size_t)k] = isDown ? 1 : 0;
		}
		down[side][0] = down[side][1];
	}
	std::vector<int> lands;
	for (int k = 1; k < n; k++)
		if (down[1][(size_t)k] && !down[1][(size_t)k - 1])
			lands.push_back(k);

	int a = -1, b = -1;
	float best = 1e9f;
	for (size_t i = 0; i + 1 < lands.size(); i++)
	{
		int s = lands[i], e = lands[i + 1];
		float T = (float)(e - s) / rate;
		if (T < 0.4f || T > 2.0f)
			continue;
		int otherLands = 0;
		for (int k = s + 1; k < e; k++)
			otherLands += down[0][(size_t)k] && !down[0][(size_t)k - 1];
		if (otherLands != 1)
			continue;
		float turn = std::fabs(facing[(size_t)e] - facing[(size_t)s]);
		if (turn > glm::radians(10.0f))
			continue;
		float score = 4.0f * turn + std::fabs(0.5f * (float)(s + e) - 0.5f * (float)n) / (float)n;
		if (score < best)
		{
			best = score;
			a = s;
			b = e;
		}
	}
	if (a < 0)
		return false;

	const int N = b - a;
	glm::vec3 travel = root[(size_t)b] - root[(size_t)a];
	travel.y = 0.0f;
	glm::quat R = glm::angleAxis(-std::atan2(travel.x, travel.z), up);
	std::vector<LocoPose> frames((size_t)N + 1);
	for (int k = 0; k <= N; k++)
	{
		float t = (float)k / (float)N;
		glm::vec3 v = root[(size_t)(a + k)] - root[(size_t)a] - travel * t;
		v.y = 0.0f;
		LocoPose& f = frames[(size_t)k];
		f.Root = R * v;
		f.Root.y = root[(size_t)(a + k)].y;
		for (int i = 0; i < LocoPose::Bodies; i++)
			f.D[i] = glm::normalize(R * turns[(size_t)(a + k)][(size_t)i]);
	}
	glm::vec3 rootError = frames[(size_t)N].Root - frames[0].Root;
	glm::quat turnError[LocoPose::Bodies];
	for (int i = 0; i < LocoPose::Bodies; i++)
	{
		glm::quat e = glm::normalize(frames[0].D[i] * glm::conjugate(frames[(size_t)N].D[i]));
		turnError[i] = e.w < 0.0f ? -e : e;
	}
	// His soles on the floor: the stride's lowest soles, his own foot boxes
	// at their corners, at a low percentile over the stride, laid on 0.
	std::vector<float> lows(sole.begin() + a, sole.begin() + b);
	std::sort(lows.begin(), lows.end());
	const float lift = -lows[lows.size() / 10];
	for (LocoPose& f : frames)
		f.Root.y += lift;

	out = LocoCycle{};
	out.Name = name;
	out.Duration = (float)N / rate;
	out.Speed = glm::length(travel) / out.Duration;
	out.Frames.resize((size_t)N);
	out.Down.resize((size_t)N * 2);
	for (int k = 0; k < N; k++)
	{
		float t = (float)k / (float)N;
		LocoPose f = frames[(size_t)k];
		f.Root -= rootError * t;
		for (int i = 0; i < LocoPose::Bodies; i++)
			f.D[i] = glm::normalize(glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), turnError[i], t) * f.D[i]);
		out.Frames[(size_t)k] = f;
		out.Down[(size_t)k * 2 + 0] = down[0][(size_t)(a + k)];
		out.Down[(size_t)k * 2 + 1] = down[1][(size_t)(a + k)];
	}
	return true;
}

// Standing: a stretch of a recording where the person stands, averaged so
// the pose is still, turned to face +z, its hips over the origin.
inline bool BakeLocoIdle(const MotionClip& clip, const Humanoid& rig, float from, float to, LocoPose& out)
{
	ClipRetarget map;
	if (!map.Setup(clip, rig))
		return false;
	MotionClip::Pose p;
	glm::vec3 P[ClipRetarget::BodyCount];
	glm::quat Q[ClipRetarget::BodyCount];
	LocoPose sum;
	float yaw = 0.0f, soleSum = 0.0f;
	int count = 0;
	for (float t = from; t <= std::min(to, clip.Duration()); t += 1.0f / 30.0f)
	{
		clip.Evaluate(t, p);
		LocoPose f;
		map.Turns(p, f.Root, f.D);
		map.PoseFrom(p, P, Q);
		soleSum += LowestSole(rig, P, Q);
		glm::vec3 fwd = glm::normalize(glm::slerp(Q[0], Q[1], 0.5f)) * glm::vec3(0.0f, 0.0f, 1.0f);
		yaw += std::atan2(fwd.x, fwd.z);
		count++;
		float w = 1.0f / (float)count;
		sum.Root = glm::mix(sum.Root, f.Root, w);
		for (int i = 0; i < LocoPose::Bodies; i++)
		{
			glm::quat q = glm::dot(sum.D[i], f.D[i]) < 0.0f ? -f.D[i] : f.D[i];
			sum.D[i] = count == 1 ? q : glm::normalize(glm::slerp(sum.D[i], q, w));
		}
	}
	if (count == 0)
		return false;
	glm::quat R = glm::angleAxis(-yaw / (float)count, glm::vec3(0.0f, 1.0f, 0.0f));
	out.Root = glm::vec3(0.0f, sum.Root.y - soleSum / (float)count, 0.0f);
	for (int i = 0; i < LocoPose::Bodies; i++)
		out.D[i] = glm::normalize(R * sum.D[i]);

	// Standing up straight. Both people recorded standing (69_01's start,
	// 16_33's end) come out on him with their knees bent 24-35 degrees --
	// how they stood, or what retargeting does to a leg that is nearly
	// straight; either way he stood in a crouch, with no reach left to step
	// with when shoved (17 of 32 shoves caught at 80 N s, against the old
	// walk's 26). Each knee is opened to 5 degrees about its own hinge, the
	// feet kept as they were turned, and the hips raised by what the legs
	// gained, so the soles stay on the floor.
	const Humanoid::RestSkeleton& rest = rig.Rest();
	float gained = 0.0f;
	for (int side = 0; side < 2; side++)
	{
		glm::vec3 thigh = rest.Knee[side] - rest.Hip[side], shin = rest.Ankle[side] - rest.Knee[side];
		glm::vec3 a = glm::normalize(out.D[3 + side] * thigh);
		glm::vec3 b = glm::normalize(out.D[5 + side] * shin);
		float bend = std::acos(glm::clamp(glm::dot(a, b), -1.0f, 1.0f));
		glm::vec3 axis = glm::cross(a, b);
		if (glm::length(axis) < 1e-5f || bend < glm::radians(5.0f))
			continue;
		glm::vec3 before = out.D[5 + side] * shin;
		out.D[5 + side] = glm::normalize(glm::angleAxis(-(bend - glm::radians(5.0f)), glm::normalize(axis)) * out.D[5 + side]);
		gained += (before.y - (out.D[5 + side] * shin).y) * 0.5f;
	}
	out.Root.y += gained;
	return true;
}

// What a humanoid's locomotion is posed from, baked for its body whenever it
// is built (Humanoid::SetLocomotionBaker), from the CMU clips in
// assets/mocap/cmu: the walk (16_15), the jog (16_35) and the run (16_55),
// one stride each, and standing from the half second before 69_01 sets off.
// None if the clips are not there, and the humanoid walks as it did before
// (DriveWalk).
//
// Found through GS::Assets, so an exported game finds the copies its export
// put beside it (GameExporter, LocomotionClipFiles) and the editor finds its
// own beside the executable.
inline const std::vector<std::string>& LocomotionClipFiles()
{
	static const std::vector<std::string> files = { "16.asf", "16_15.amc", "16_35.amc", "16_55.amc", "69.asf", "69_01.amc", "clips.txt" };
	return files;
}

inline void AttachLocomotion(Humanoid& rig, const std::string& folder = "assets/mocap/cmu")
{
	rig.SetLocomotionBaker([folder](Humanoid& r)
	{
		static std::map<std::string, MotionClip> cache;
		auto clip = [&](const char* skeleton, const char* frames) -> const MotionClip*
		{
			std::string key = std::string(skeleton) + "/" + frames;
			auto it = cache.find(key);
			if (it == cache.end())
			{
				MotionClip c;
				if (!c.Load(GS::Assets::Resolve(folder + "/" + skeleton), GS::Assets::Resolve(folder + "/" + frames)))
					return nullptr;
				it = cache.emplace(key, std::move(c)).first;
			}
			return &it->second;
		};
		auto clips = std::make_shared<LocoClips>();
		const struct { const char* Name; const char* Frames; } strides[] = {
			{ "walk", "16_15.amc" }, { "jog", "16_35.amc" }, { "run", "16_55.amc" } };
		for (const auto& s : strides)
		{
			LocoCycle cycle;
			if (const MotionClip* c = clip("16.asf", s.Frames); c && BakeLocoCycle(*c, r, s.Name, cycle))
				clips->Cycles.push_back(std::move(cycle));
		}
		const MotionClip* standing = clip("69.asf", "69_01.amc");
		if (clips->Cycles.empty() || !standing || !BakeLocoIdle(*standing, r, 0.1f, 0.6f, clips->Idle))
		{
			r.SetLocomotionClips(nullptr);
			return;
		}
		std::sort(clips->Cycles.begin(), clips->Cycles.end(),
			[](const LocoCycle& x, const LocoCycle& y) { return x.Speed < y.Speed; });
		r.SetLocomotionClips(clips);
	});
}
