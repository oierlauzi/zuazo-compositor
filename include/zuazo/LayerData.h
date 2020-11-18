#pragma once

#include "Processors/Compositor.h"

#include <zuazo/Macros.h>
#include <zuazo/Graphics/CommandBuffer.h>
#include <zuazo/Math/Vector.h>
#include <zuazo/Signal/NamingConventions.h>

#include <memory>

namespace Zuazo {

class LayerData {
public:
	using CommandBuffer = std::shared_ptr<const Graphics::CommandBuffer>;
	using RenderingStage = Processors::Compositor::RenderingStage;

	LayerData() = default;
	LayerData(const LayerData& other) = default;
	LayerData(LayerData&& other) = default;
	~LayerData() = default;

	LayerData&				operator=(const LayerData& other) = default;
	LayerData&				operator=(LayerData&& other) = default;

	void					setCommandBuffer(CommandBuffer cmd);
	const CommandBuffer&	getCommandBuffer() const;

	void					setAveragePosition(const Math::Vec4f& pos);
	const Math::Vec4f&		getAveragePosition() const;

	void					setRenderingStage(RenderingStage stage);
	RenderingStage			getRenderingStage() const;

	void					setHasAlpha(bool alpha);
	bool					getHasAlpha() const;

private:
	CommandBuffer			m_commandBuffer;
	Math::Vec4f				m_averagePosition;
	RenderingStage			m_stage;
	bool					m_hasAlpha;

};

using LayerDataStream = std::shared_ptr<const LayerData>;



namespace Signal {

template<>
constexpr std::string_view makeInputName<LayerDataStream>() noexcept { return "layer"; }

template<>
constexpr std::string_view makeOutputName<LayerDataStream>() noexcept { return "layer"; }

}

}

#include "LayerData.inl"