#pragma once

#include "egsspch.h"
#include "Core.h"
#include "Layer.h"

namespace Egss {

	// Layers live in the first half of the vector, overlays in the second.
	// m_LayerInsertIndex marks the boundary, so overlays always stay on top
	// regardless of push order.
	//
	// Updates run bottom-to-top; events are dispatched top-to-bottom.
	class EGSS_API LayerStack
	{
	public:
		LayerStack();
		~LayerStack();

		void PushLayer(Layer* layer);
		void PushOverlay(Layer* overlay);
		void PopLayer(Layer* layer);
		void PopOverlay(Layer* overlay);

		std::vector<Layer*>::iterator begin() { return m_Layers.begin(); }
		std::vector<Layer*>::iterator end() { return m_Layers.end(); }
		std::vector<Layer*>::reverse_iterator rbegin() { return m_Layers.rbegin(); }
		std::vector<Layer*>::reverse_iterator rend() { return m_Layers.rend(); }
	private:
		std::vector<Layer*> m_Layers;
		unsigned int m_LayerInsertIndex = 0;
	};

}
