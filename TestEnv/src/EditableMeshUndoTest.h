// TEMPORARY -- delete after verifying EditableMesh's undo/redo.
#pragma once
#include <GS.h>

#include "EditableMesh.h"

namespace EditableMeshUndoTest {

	inline int g_Pass = 0, g_Fail = 0;
	inline void Check(bool ok, const std::string& what)
	{
		ok ? g_Pass++ : g_Fail++;
		GS_TRACE("  [{0}] {1}", ok ? "ok " : "FAIL", what);
	}

	inline void Run()
	{
		EditableMesh mesh = EditableMesh::FromMeshData(GS::Mesh::CreateCubeData(1.0f));

		mesh.PushUndo();
		mesh.MovePoint(0, { 9.0f, 9.0f, 9.0f });
		Check(mesh.Point(0).Position == glm::vec3(9.0f, 9.0f, 9.0f), "move applied");

		mesh.PushUndo();
		mesh.DeleteFace(0);
		Check(mesh.FaceCount() == 5, "delete applied (6 -> 5)");

		mesh.Undo();
		Check(mesh.FaceCount() == 6, "undo reverts the delete (5 -> 6)");
		Check(mesh.Point(0).Position == glm::vec3(9.0f, 9.0f, 9.0f), "the earlier move is still in effect after undoing the delete");

		mesh.Undo();
		Check(mesh.Point(0).Position != glm::vec3(9.0f, 9.0f, 9.0f), "a second undo reverts the move too");
		Check(!mesh.CanUndo(), "nothing left to undo");

		mesh.Redo();
		mesh.Redo();
		Check(mesh.FaceCount() == 5, "redoing both steps re-applies the delete (back to 5 faces)");
		Check(!mesh.CanRedo(), "nothing left to redo");

		GS_TRACE("EditableMeshUndoTest: {0} passed, {1} failed", g_Pass, g_Fail);
	}

}
