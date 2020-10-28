#include "LayerData.h"

namespace Zuazo {

inline void LayerData::setCommandBuffer(CommandBuffer cmd) {
	m_commandBuffer = std::move(cmd);
}

inline const LayerData::CommandBuffer& LayerData::getCommandBuffer() const {
	return m_commandBuffer;
}


inline void LayerData::setAveragePosition(const Math::Vec4f& pos) {
	m_averagePosition = pos;
}

inline const Math::Vec4f& LayerData::getAveragePosition() const {
	return m_averagePosition;
}


inline void LayerData::setRenderingStage(RenderingStage stage) {
	m_stage = stage;
}

inline LayerData::RenderingStage LayerData::getRenderingStage() const {
	return m_stage;
}


inline void LayerData::setHasAlpha(bool alpha) {
	m_hasAlpha = alpha;
}

inline bool LayerData::getHasAlpha() const {
	return m_hasAlpha;
}


}

