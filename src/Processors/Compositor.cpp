#include <zuazo/Processors/Compositor.h>

#include <zuazo/LayerData.h>
#include <zuazo/Graphics/CommandBuffer.h>
#include <zuazo/Graphics/StagedBuffer.h>
#include <zuazo/Graphics/Drawtable.h>
#include <zuazo/Graphics/CommandBufferPool.h>
#include <zuazo/Signal/Input.h>
#include <zuazo/Signal/Output.h>
#include <zuazo/Utils/Pool.h>
#include <zuazo/Utils/StaticId.h>


#include <memory>
#include <vector>
#include <utility>
#include <algorithm>
#include <tuple>
#include <bitset>

namespace Zuazo::Processors {

/*
 * CompositorImpl
 */

struct CompositorImpl {
	struct Open {
		enum DescriptorLayouts {
			DESCRIPTOR_SET_COMPOSITOR,

			DESCRIPTOR_SET_COUNT
		};

		using UniformBufferLayout = std::array<Utils::Area, RendererBase::DESCRIPTOR_COUNT>;

		using CommandPool = Utils::Pool<Graphics::CommandBuffer>;
		using Layers = std::vector<std::reference_wrapper<const LayerData>>;

		struct Cache {
			std::vector<vk::CommandBuffer>				drawCommandBuffers;
			std::vector<std::shared_ptr<const void>>	dependencies;
		};

		const Graphics::Vulkan& 					vulkan;

		UniformBufferLayout							uniformBufferLayout;
		Graphics::StagedBuffer						uniformBuffer;
		vk::UniqueDescriptorPool					descriptorPool;
		vk::DescriptorSet							descriptorSet;
		vk::PipelineLayout							pipelineLayout;

		Graphics::Drawtable 						drawtable;
		Graphics::OutputColorTransfer				colorTransfer;
		Graphics::CommandBufferPool					commandBufferPool;
		
		std::vector<vk::ClearValue>					clearValues;

		std::shared_ptr<const Graphics::Frame>		lastFrame;

		Layers										layers;
		Utils::Area									uniformFlushArea;
		vk::PipelineStageFlags						uniformFlushStages;
		Cache										cache;

		Open(	const Graphics::Vulkan& vulkan, 
				const Graphics::Frame::Descriptor& frameDesc,
				DepthStencilFormat depthStencilFmt,
				const Compositor::Camera& cam )
			: vulkan(vulkan)
			, uniformBufferLayout(createUniformBufferLayout(vulkan))
			, uniformBuffer(createUniformBuffer(vulkan, uniformBufferLayout))
			, descriptorPool(createDescriptorPool(vulkan))
			, descriptorSet(createDescriptorSet(vulkan, *descriptorPool))
			, pipelineLayout(createPipelineLayout(vulkan))

			, drawtable(createDrawtable(vulkan, frameDesc, depthStencilFmt))
			, colorTransfer(drawtable.getOutputColorTransfer())
			, commandBufferPool(createCommandBufferPool(vulkan))

			, clearValues(Graphics::Drawtable::getClearValues(frameDesc, depthStencilFmt))
		{
			//Bind the uniform buffers to the descriptor sets
			writeDescriptorSets();

			//Update the contents of the uniforms buffers
			updateProjectionMatrixUniform(cam);
			updateColorTransferUniform();
		}

		Open(const Open& other) = delete;

		~Open() {
			uniformBuffer.waitCompletion(vulkan);
		}

		void recreate(	const Graphics::Frame::Descriptor& frameDesc,
						DepthStencilFormat depthStencilFmt,
						const Compositor::Camera& cam )
		{
			enum {
				RECREATE_DRAWTABLE,
				RECREATE_CLEAR_VALUES,
				UPDATE_PROJECTION_MATRIX,
				UPDATE_COLOR_TRANSFER,

				MODIFICATION_COUNT
			};

			std::bitset<MODIFICATION_COUNT> modifications;

			modifications.set(RECREATE_DRAWTABLE); //Best guess
			modifications.set(UPDATE_PROJECTION_MATRIX, drawtable.getFrameDescriptor().calculateSize() != frameDesc.calculateSize());

			//Evaluate which modifications need to be done
			if(modifications.test(RECREATE_DRAWTABLE)) {
				drawtable = Graphics::Drawtable(drawtable.getVulkan(), frameDesc, depthStencilFmt);
				modifications.set(RECREATE_CLEAR_VALUES); //Best guess
				modifications.set(UPDATE_COLOR_TRANSFER); //Best guess
			}

			if(modifications.test(RECREATE_CLEAR_VALUES)) {
				clearValues = Graphics::Drawtable::getClearValues(frameDesc, depthStencilFmt);
			}

			if(modifications.test(UPDATE_PROJECTION_MATRIX)) {
				updateProjectionMatrixUniform(cam);
			}

			if(modifications.test(UPDATE_COLOR_TRANSFER)) {
				updateColorTransferUniform();
			}
		}

		void setCamera(const Compositor::Camera& cam) {
			updateProjectionMatrixUniform(cam);
		}

		void addLayer(const LayerData& layer) {
			layers.push_back(layer);
		}

		Video draw() {
			//Cache should have been cleared
			assert(cache.drawCommandBuffers.empty());
			assert(cache.dependencies.empty());

			//Obtain a new frame and command buffer
			auto result = drawtable.acquireFrame();
			auto commandBuffer = commandBufferPool.acquireCommandBuffer();

			//Sort the layers based on their alpha and depth. Stable sort is used to preserve order
			std::stable_sort(
				layers.begin(), layers.end(),
				std::bind(&Open::layerComp, std::cref(*this), std::placeholders::_1, std::placeholders::_2)
			);

			//Obtain all the vulkan command buffers and add them as a dependency
			for(const auto& layer : layers) {
				const auto& commandBuffer = layer.get().getCommandBuffer();
				cache.drawCommandBuffers.push_back(commandBuffer->getCommandBuffer());
				cache.dependencies.push_back(commandBuffer);
			}



			//Begin the commandbuffer
			constexpr vk::CommandBufferBeginInfo cmdBeginInfo(
				vk::CommandBufferUsageFlagBits::eOneTimeSubmit
			);
			commandBuffer->begin(cmdBeginInfo);

			//Bind all descriptors
			commandBuffer->bindDescriptorSets(
				vk::PipelineBindPoint::eGraphics,								//Pipeline bind point
				pipelineLayout,													//Pipeline layout
				DESCRIPTOR_SET_COMPOSITOR,									//First index
				descriptorSet,													//Descriptor sets
				{}																//Dynamic offsets
			);

			//Set the dynamic viewport
			const auto extent = Graphics::toVulkan(drawtable.getFrameDescriptor().getResolution());
			const std::array viewports = {
				vk::Viewport(
					0.0f, 			0.0f,
					extent.width, 	extent.height,
					0.0f,			1.0f
				)
			};
			commandBuffer->setViewport(0, viewports);

			//Set the dynamic scissor
			const std::array scissors = {
				vk::Rect2D(
					vk::Offset2D(0, 0),
					extent
				)
			};
			commandBuffer->setScissor(0, scissors);

			//Draw to the command buffer
			result->beginRenderPass(
				commandBuffer->getCommandBuffer(),
				scissors.front(),
				clearValues, 
				vk::SubpassContents::eSecondaryCommandBuffers //We'll only call execute:
			);

			//Execute all the command buffers gathered from the layers
			if(!cache.drawCommandBuffers.empty()) {
				//Flush the uniform buffer, as it will be used
				uniformBuffer.flushData(
					vulkan,
					uniformFlushArea,
					vulkan.getGraphicsQueueIndex(),
					vk::AccessFlagBits::eUniformRead,
					uniformFlushStages
				);
				uniformFlushArea = {};
				uniformFlushStages = {};

				//Execute all the command buffers
				commandBuffer->execute(cache.drawCommandBuffers); 
			}

			result->endRenderPass(commandBuffer->getCommandBuffer());
			commandBuffer->end();

			//Add dependencies to the command buffer
			commandBuffer->setDependencies(cache.dependencies);

			//Draw to the frame
			result->draw(std::move(commandBuffer));

			//Clear the state. This should not deallocate them
			layers.clear(); 
			cache.drawCommandBuffers.clear();
			cache.dependencies.clear();

			lastFrame = result;
			return result;
		}

	private:
		bool layerComp(const LayerData& a, const LayerData& b) const {
			/*
			 * The strategy will be the following:
			 * 1. Draw the alphaless objects backwards, writing and testing depth
			 * 2. Draw the transparent objscts forwards, writing and testing depth
			 */
			
			if(a.getHasAlpha() != b.getHasAlpha()) {
				//Prioritize alphaless drawing
				return a.getHasAlpha() < b.getHasAlpha();
			} else {
				//Both or neither have alpha. Depth must be taken in consideration
				assert(a.getHasAlpha() == b.getHasAlpha());
				const auto hasAlpha = a.getHasAlpha();

				//Calculate the average depth
				const auto& projMatrix = *(reinterpret_cast<const Math::Mat4x4f*>(uniformBufferLayout[RendererBase::DESCRIPTOR_BINDING_PROJECTION_MATRIX].begin(uniformBuffer.data())));
				const auto aDepth = (projMatrix * a.getAveragePosition()).z;
				const auto bDepth = (projMatrix * b.getAveragePosition()).z;

				return !hasAlpha
					? aDepth < bDepth
					: aDepth > bDepth;
			}
		}

		void updateProjectionMatrixUniform(const Compositor::Camera& cam) {
			waitRenderCompletion();
			uniformBuffer.waitCompletion(vulkan);

			auto& mtx = *(reinterpret_cast<Math::Mat4x4f*>(uniformBufferLayout[RendererBase::DESCRIPTOR_BINDING_PROJECTION_MATRIX].begin(uniformBuffer.data())));
			mtx = cam.calculateMatrix(drawtable.getFrameDescriptor().calculateSize());
			
			uniformFlushArea |= uniformBufferLayout[RendererBase::DESCRIPTOR_BINDING_PROJECTION_MATRIX];
			uniformFlushStages |= vk::PipelineStageFlagBits::eVertexShader;
		}

		void updateColorTransferUniform() {
			waitRenderCompletion();
			uniformBuffer.waitCompletion(vulkan);
			
			std::memcpy(
				uniformBufferLayout[RendererBase::DESCRIPTOR_BINDING_COLOR_TRANSFER].begin(uniformBuffer.data()),
				colorTransfer.data(),
				uniformBufferLayout[RendererBase::DESCRIPTOR_BINDING_COLOR_TRANSFER].size()
			);

			uniformFlushArea |= uniformBufferLayout[RendererBase::DESCRIPTOR_BINDING_COLOR_TRANSFER];
			uniformFlushStages |= vk::PipelineStageFlagBits::eFragmentShader;
		}

		void writeDescriptorSets() {
			const std::array projectionMatrixBuffers = {
				vk::DescriptorBufferInfo(
					uniformBuffer.getBuffer(),															//Buffer
					uniformBufferLayout[RendererBase::DESCRIPTOR_BINDING_PROJECTION_MATRIX].offset(),	//Offset
					uniformBufferLayout[RendererBase::DESCRIPTOR_BINDING_PROJECTION_MATRIX].size()		//Size
				)
			};
			const std::array colorTransferBuffers = {
				vk::DescriptorBufferInfo(
					uniformBuffer.getBuffer(),														//Buffer
					uniformBufferLayout[RendererBase::DESCRIPTOR_BINDING_COLOR_TRANSFER].offset(),	//Offset
					uniformBufferLayout[RendererBase::DESCRIPTOR_BINDING_COLOR_TRANSFER].size()		//Size
				)
			};

			const std::array writeDescriptorSets = {
				vk::WriteDescriptorSet( //Viewport UBO
					descriptorSet,											//Descriptor set
					RendererBase::DESCRIPTOR_BINDING_PROJECTION_MATRIX,		//Binding
					0, 														//Index
					projectionMatrixBuffers.size(),							//Descriptor count		
					vk::DescriptorType::eUniformBuffer,						//Descriptor type
					nullptr, 												//Images 
					projectionMatrixBuffers.data(), 						//Buffers
					nullptr													//Texel buffers
				),
				vk::WriteDescriptorSet( //ColorTransfer UBO
					descriptorSet,											//Descriptor set
					RendererBase::DESCRIPTOR_BINDING_COLOR_TRANSFER,		//Binding
					0, 														//Index
					colorTransferBuffers.size(),							//Descriptor count		
					vk::DescriptorType::eUniformBuffer,						//Descriptor type
					nullptr, 												//Images 
					colorTransferBuffers.data(), 							//Buffers
					nullptr													//Texel buffers
				)
			};

			vulkan.updateDescriptorSets(writeDescriptorSets, {});
		}

		void waitRenderCompletion() {
			if(lastFrame) {
				lastFrame->waitDependencies();
			}
		}

		static vk::RenderPass createRenderPass(	const Graphics::Vulkan& vulkan, 
												const Graphics::Frame::Descriptor& frameDesc,
												DepthStencilFormat depthStencilFmt ) 
		{
			return Graphics::Drawtable::getRenderPass(vulkan, frameDesc, depthStencilFmt);
		}

		static UniformBufferLayout createUniformBufferLayout(const Graphics::Vulkan& vulkan) {
			const auto& limits = vulkan.getPhysicalDeviceProperties().limits;

			constexpr size_t projectionMatrixOff = 0;
			constexpr size_t projectionMatrixSize = sizeof(glm::mat4);
			
			const size_t colorTansferOff = Utils::align(
				projectionMatrixOff + projectionMatrixSize, 
				limits.minUniformBufferOffsetAlignment
			);
			const size_t colorTansferSize = Graphics::OutputColorTransfer::size();

			return UniformBufferLayout {
				Utils::Area(projectionMatrixOff,	projectionMatrixSize),	//Projection matrix
				Utils::Area(colorTansferOff,		colorTansferSize )		//Color Transfer
			};
		}

		static Graphics::StagedBuffer createUniformBuffer(const Graphics::Vulkan& vulkan, const UniformBufferLayout& layout) {
			return Graphics::StagedBuffer(
				vulkan,
				vk::BufferUsageFlagBits::eUniformBuffer,
				layout.back().end()
			);
		}

		static vk::UniqueDescriptorPool createDescriptorPool(const Graphics::Vulkan& vulkan){
			const std::array poolSizes = {
				vk::DescriptorPoolSize(
					vk::DescriptorType::eUniformBuffer,					//Descriptor type
					RendererBase::DESCRIPTOR_COUNT						//Descriptor count
				)
			};

			const vk::DescriptorPoolCreateInfo createInfo(
				{},														//Flags
				1,														//Descriptor set count
				poolSizes.size(), poolSizes.data()						//Pool sizes
			);

			return vulkan.createDescriptorPool(createInfo);
		}

		static vk::DescriptorSet createDescriptorSet(	const Graphics::Vulkan& vulkan,
														vk::DescriptorPool pool )
		{
			const auto layout = RendererBase::getDescriptorSetLayout(vulkan);
			return vulkan.allocateDescriptorSet(pool, layout).release();
		}

		static vk::PipelineLayout createPipelineLayout(const Graphics::Vulkan& vulkan) {			
			//This pipeline layout won't be used to create any pipeline, but it must be compatible with the
			// 1st descriptor set of all the pipelines, so that the color transfer and projection-view matrices
			// are bound.
			static const Utils::StaticId id;

			auto result = vulkan.createPipelineLayout(id);

			if(!result) {
				const std::array layouts {
					RendererBase::getDescriptorSetLayout(vulkan)
				};

				const vk::PipelineLayoutCreateInfo createInfo(
					{},													//Flags
					layouts.size(), layouts.data(),						//Descriptor set layouts
					0, nullptr											//Push constants
				);

				result = vulkan.createPipelineLayout(id, createInfo);
			}

			return result;
		}



		static Graphics::Drawtable createDrawtable(	const Graphics::Vulkan& vulkan, 
													const Graphics::Frame::Descriptor& desc,
													DepthStencilFormat depthStencilFmt )
		{
			return Graphics::Drawtable(
				vulkan,
				desc,
				depthStencilFmt
			);
		}

		static Graphics::CommandBufferPool createCommandBufferPool(const Graphics::Vulkan& vulkan) {
			constexpr vk::CommandPoolCreateFlags flags =
				vk::CommandPoolCreateFlagBits::eResetCommandBuffer |	//Command buffers will be reset individually
				vk::CommandPoolCreateFlagBits::eTransient ;				//Command buffers will be reset often

			return Graphics::CommandBufferPool(
				vulkan,
				flags,
				vulkan.getGraphicsQueueIndex(),
				vk::CommandBufferLevel::ePrimary
			);
		}
	};

	using LayerInput = Signal::Input<LayerDataStream>;
	using Output = Signal::Output<Video>;

	std::reference_wrapper<Compositor> 			owner;

	std::vector<LayerInput>						layerIns;
	Output										videoOut;

	std::unique_ptr<Open>						opened;
	bool										hasChanged;

	CompositorImpl(	Compositor& comp )
		: owner(comp)
		, layerIns()
		, videoOut(std::string(Signal::makeOutputName<Video>()), createPullCallback(this))
	{
	}

	~CompositorImpl() = default;


	void moved(ZuazoBase& base) {
		owner = static_cast<Compositor&>(base);
	}

	void open(ZuazoBase& base) {
		assert(!opened);
		auto& compositor = static_cast<Compositor&>(base);
		assert(&owner.get() == &compositor);

		if(static_cast<bool>(compositor.getVideoMode()) && static_cast<bool>(compositor.getDepthStencilFormat())) {
			opened = Utils::makeUnique<Open>(
				compositor.getInstance().getVulkan(),
				compositor.getVideoMode().getFrameDescriptor(),
				compositor.getDepthStencilFormat().value(),
				compositor.getCamera()
			);
		}

		hasChanged = true; //Signal rendering if needed
	}

	void close(ZuazoBase& base) {
		auto& compositor = static_cast<Compositor&>(base);
		assert(&owner.get() == &compositor);
		
		videoOut.reset();
		opened.reset();
	}

	void update() {
		auto& compositor = owner.get();

		if(opened) {
			const bool inputsHaveChanged = std::any_of(
				layerIns.cbegin(), layerIns.cend(),
				[] (const LayerInput& input) -> bool {
					return input.hasChanged();
				}
			);

			if(hasChanged || inputsHaveChanged) {
				if(compositor.getVideoMode()) {
					//Query all the layers
					for(auto& layerIn : layerIns) {
						const auto& layerData = layerIn.pull();

						//Draw only if valid
						if(layerData){
							opened->addLayer(*layerData);
						} 
					}

					videoOut.push(opened->draw());
				} else {
					videoOut.reset();
				}

				//Update the state
				hasChanged = false;
			}
		}
	}

	std::vector<VideoMode> getVideoModeCompatibility() const {
		const auto& compositor = owner.get();
		std::vector<VideoMode> result;

		//Normal formats
		result.emplace_back(
			Utils::MustBe<Rate>(Rate(0, 1)),
			compositor.getInstance().getResolutionSupport(),
			Utils::Any<AspectRatio>(),
			Utils::Any<ColorPrimaries>(),
			Utils::Any<ColorModel>(),
			Utils::MustBe<ColorTransferFunction>(ColorTransferFunction::LINEAR),
			Utils::MustBe<ColorSubsampling>(ColorSubsampling::RB_444),
			Utils::Any<ColorRange>(),
			Graphics::Drawtable::getSupportedFormats(compositor.getInstance().getVulkan())
		);

		//sRGB formats
		result.emplace_back(
			Utils::MustBe<Rate>(Rate(0, 1)),
			compositor.getInstance().getResolutionSupport(),
			Utils::Any<AspectRatio>(),
			Utils::Any<ColorPrimaries>(),
			Utils::MustBe<ColorModel>(ColorModel::RGB),
			Utils::MustBe<ColorTransferFunction>(ColorTransferFunction::IEC61966_2_1),
			Utils::MustBe<ColorSubsampling>(ColorSubsampling::RB_444),
			Utils::MustBe<ColorRange>(ColorRange::FULL),
			Graphics::Drawtable::getSupportedSrgbFormats(compositor.getInstance().getVulkan())
		);

		return result;
	}
	
	Utils::Limit<DepthStencilFormat> getDepthStencilFormatCompatibility() const {
		const auto& compositor = owner.get();
		return Graphics::Drawtable::getSupportedFormatsDepthStencil(compositor.getInstance().getVulkan());
	}

	void recreateCallback(	Compositor& compositor, 
							const VideoMode& videoMode, 
							const Utils::Limit<DepthStencilFormat>& depthStencilFormat )
	{
		assert(&owner.get() == &compositor);

		if(compositor.isOpen()) {
			const auto isValid = static_cast<bool>(videoMode) && static_cast<bool>(depthStencilFormat);

			if(opened && isValid) {
				//Video mode remains valid
				opened->recreate(
					videoMode.getFrameDescriptor(),
					depthStencilFormat.value(),
					compositor.getCamera()
				);
			} else if(opened && !isValid) {
				//Video mode is not valid anymore
				opened.reset();
				videoOut.reset();
			} else if(!opened && isValid) {
				//Video mode has become valid
				opened = Utils::makeUnique<Open>(
					compositor.getInstance().getVulkan(),
					videoMode.getFrameDescriptor(),
					depthStencilFormat.value(),
					compositor.getCamera()
				);
			}
		}

		hasChanged = true;
	}

	void videoModeCallback(VideoBase& base, const VideoMode& videoMode) {
		auto& compositor = static_cast<Compositor&>(base);
		recreateCallback(compositor, videoMode, compositor.getDepthStencilFormat());
	}

	void depthStencilCallback(RendererBase& base, const Utils::Limit<DepthStencilFormat>& depthStencilFormat) {
		auto& compositor = static_cast<Compositor&>(base);
		recreateCallback(compositor, compositor.getVideoMode(), depthStencilFormat);
	}

	void cameraCallback(RendererBase& base, const Compositor::Camera& cam) {
		auto& compositor = static_cast<Compositor&>(base);
		assert(&owner.get() == &compositor); (void)compositor;

		if(opened) {
			opened->setCamera(cam);
		}
	}

	vk::RenderPass renderPassQueryCallback(const RendererBase& base) {
		const auto& compositor = static_cast<const Compositor&>(base);
		assert(&owner.get() == &compositor); 

		vk::RenderPass result = {};

		const auto& videoMode = compositor.getVideoMode();
		const auto& depthStencilFormat = compositor.getDepthStencilFormat();

		if(videoMode && depthStencilFormat) {
			const auto& vulkan = compositor.getInstance().getVulkan();
			const auto frameDesc = videoMode.getFrameDescriptor();
			const auto depthStencilFmt = depthStencilFormat.value();

			result = Graphics::Drawtable::getRenderPass(vulkan, frameDesc, depthStencilFmt);
		}

		return result;
	}

	void setLayerCount(size_t count) {
		auto& compositor = owner.get();

		//Unregister all pads as they might be reallocated
		for(auto& pad : layerIns) {
			compositor.removePad(pad);
		}

		//Resize the layer vector
		const auto oldSize = getLayerCount();
		layerIns.resize(count, LayerInput(""));

		//Rename the newly created layers according to their index
		for(size_t i = oldSize; i < layerIns.size(); ++i) {
			layerIns[i].setName(Signal::makeInputName<LayerDataStream>(i));
		}

		//Register all pads again
		for(auto& pad : layerIns) {
			compositor.registerPad(pad);
		}

		hasChanged = true;
	}

	size_t getLayerCount() const {
		return layerIns.size();
	}

private:
	static Output::PullCallback createPullCallback(CompositorImpl* impl) {
		return [impl] (Output&) {
			impl->owner.get().update();
		};
	}

};



/*
 * Compositor
 */

Compositor::Compositor(	Instance& instance, 
						std::string name, 
						VideoMode videoMode,
						Utils::Limit<DepthStencilFormat> depthStencil )
	: Utils::Pimpl<CompositorImpl>({}, *this)
	, ZuazoBase(
		instance, 
		std::move(name),
		PadRef((*this)->videoOut),
		std::bind(&CompositorImpl::moved, std::ref(**this), std::placeholders::_1),
		std::bind(&CompositorImpl::open, std::ref(**this), std::placeholders::_1),
		std::bind(&CompositorImpl::close, std::ref(**this), std::placeholders::_1),
		std::bind(&CompositorImpl::update, std::ref(**this)) )
	, VideoBase(
		std::move(videoMode),
		std::bind(&CompositorImpl::videoModeCallback, std::ref(**this), std::placeholders::_1, std::placeholders::_2) )
	, RendererBase(
		std::move(depthStencil),
		std::bind(&CompositorImpl::depthStencilCallback, std::ref(**this), std::placeholders::_1, std::placeholders::_2),
		std::bind(&CompositorImpl::cameraCallback, std::ref(**this), std::placeholders::_1, std::placeholders::_2),
		std::bind(&CompositorImpl::renderPassQueryCallback, std::ref(**this), std::placeholders::_1) )
	, Signal::SourceLayout<Video>(makeProxy((*this)->videoOut))
{
	setVideoModeCompatibility((*this)->getVideoModeCompatibility());
	setDepthStencilFormatCompatibility((*this)->getDepthStencilFormatCompatibility());
}

Compositor::Compositor(Compositor&& other) = default;

Compositor::~Compositor() = default;

Compositor& Compositor::operator=(Compositor&& other) = default;


void Compositor::setLayerCount(size_t count) {
	(*this)->setLayerCount(count);
}

size_t Compositor::getLayerCount() const {
	return (*this)->getLayerCount();
}

}
