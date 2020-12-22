#include <zuazo/Processors/Compositor.h>

#include <zuazo/LayerBase.h>
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

		using CommandPool = Utils::Pool<Graphics::CommandBuffer>;

		struct Resources {
			Resources(	Graphics::StagedBuffer uniformBuffer,
						vk::UniqueDescriptorPool descriptorPool )
				: uniformBuffer(std::move(uniformBuffer))
				, descriptorPool(std::move(descriptorPool))
			{
			}

			~Resources() = default;

			Graphics::StagedBuffer								uniformBuffer;
			vk::UniqueDescriptorPool							descriptorPool;
		};

		struct Cache {
			std::vector<Compositor::LayerRef>			layers;
		};

		const Graphics::Vulkan& 					vulkan;

		RendererBase::UniformBufferLayout			uniformBufferLayout;
		std::shared_ptr<Resources>					resources;
		vk::DescriptorSet							descriptorSet;
		vk::PipelineLayout							pipelineLayout;

		Graphics::Drawtable 						drawtable;
		Graphics::CommandBufferPool					commandBufferPool;
		
		std::vector<vk::ClearValue>					clearValues;

		Math::Mat4x4f&								uniformProjectionMatrix;
		Utils::BufferView<std::byte>				uniformOutputColorTransfer;
		Utils::Area									uniformFlushArea;
		vk::PipelineStageFlags						uniformFlushStages;

		Open(	const Graphics::Vulkan& vulkan, 
				const Graphics::Frame::Descriptor& frameDesc,
				DepthStencilFormat depthStencilFmt,
				const Compositor::Camera& cam )
			: vulkan(vulkan)
			, uniformBufferLayout(RendererBase::getUniformBufferLayout(vulkan))
			, resources(Utils::makeShared<Resources>(	createUniformBuffer(vulkan, uniformBufferLayout),
														createDescriptorPool(vulkan) ))
			, descriptorSet(createDescriptorSet(vulkan, *(resources->descriptorPool)))
			, pipelineLayout(RendererBase::getPipelineLayout(vulkan))

			, drawtable(createDrawtable(vulkan, frameDesc, depthStencilFmt))
			, commandBufferPool(createCommandBufferPool(vulkan))

			, clearValues(Graphics::Drawtable::getClearValues(frameDesc, depthStencilFmt))

			, uniformProjectionMatrix(getProjectionMatrix(uniformBufferLayout, resources->uniformBuffer))
			, uniformOutputColorTransfer(getOutputColorTransfer(uniformBufferLayout, resources->uniformBuffer))
		{
			//Bind the uniform buffers to the descriptor sets
			writeDescriptorSets();

			//Update the contents of the uniforms buffers
			updateProjectionMatrixUniform(cam);
			updateColorTransferUniform();
		}

		~Open() {
			resources->uniformBuffer.waitCompletion(vulkan);
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

		Video draw(const RendererBase& renderer) {
			//Obtain the viewports and the scissors
			const auto extent = Graphics::toVulkan(drawtable.getFrameDescriptor().getResolution());
			const std::array viewports = {
				vk::Viewport(
					0.0f, 			0.0f,
					extent.width, 	extent.height,
					0.0f,			1.0f
				)
			};
			const std::array scissors = {
				vk::Rect2D(
					vk::Offset2D(0, 0),
					extent
				)
			};

			//Obtain a new frame and command buffer
			auto result = drawtable.acquireFrame();
			auto commandBuffer = commandBufferPool.acquireCommandBuffer();

			//Add the compositor related dependencies to it
			commandBuffer->addDependencies({resources});

			//Begin the commandbuffer
			constexpr vk::CommandBufferBeginInfo cmdBeginInfo(
				vk::CommandBufferUsageFlagBits::eOneTimeSubmit
			);
			commandBuffer->begin(cmdBeginInfo);

			//Draw to the command buffer
			result->beginRenderPass(
				commandBuffer->getCommandBuffer(),
				scissors.front(),
				clearValues, 
				vk::SubpassContents::eInline
			);	

			//Execute all the command buffers gathered from the layers
			if(!renderer.getLayers().empty()) {
				//Flush the uniform buffer, as it will be used
				resources->uniformBuffer.flushData(
					vulkan,
					uniformFlushArea,
					vulkan.getGraphicsQueueIndex(),
					vk::AccessFlagBits::eUniformRead,
					uniformFlushStages
				);
				uniformFlushArea = {};
				uniformFlushStages = {};

				//Bind all descriptors
				commandBuffer->bindDescriptorSets(
					vk::PipelineBindPoint::eGraphics,								//Pipeline bind point
					pipelineLayout,													//Pipeline layout
					DESCRIPTOR_SET_COMPOSITOR,										//First index
					descriptorSet,													//Descriptor sets
					{}																//Dynamic offsets
				);

				//Set the dynamic viewport and scissor
				commandBuffer->setViewport(0, viewports);
				commandBuffer->setScissor(0, scissors);

				//Record all layers
				renderer.draw(*commandBuffer);
			}

			//Finish the command buffer
			result->endRenderPass(commandBuffer->getCommandBuffer());
			commandBuffer->end();

			//Draw to the frame
			result->draw(std::move(commandBuffer));

			return result;
		}

	private:
		void updateProjectionMatrixUniform(const Compositor::Camera& cam) {
			resources->uniformBuffer.waitCompletion(vulkan);

			const auto size = drawtable.getFrameDescriptor().calculateSize();
			uniformProjectionMatrix = cam.calculateMatrix(size);
			
			uniformFlushArea |= uniformBufferLayout[RendererBase::DESCRIPTOR_BINDING_PROJECTION_MATRIX];
			uniformFlushStages |= vk::PipelineStageFlagBits::eVertexShader;
		}

		void updateColorTransferUniform() {
			resources->uniformBuffer.waitCompletion(vulkan);
			
			const auto outputColorTransfer = drawtable.getOutputColorTransfer();
			assert(outputColorTransfer.size() == uniformOutputColorTransfer.size());
			std::memcpy(
				uniformOutputColorTransfer.data(),
				outputColorTransfer.data(),
				uniformOutputColorTransfer.size()
			);

			uniformFlushArea |= uniformBufferLayout[RendererBase::DESCRIPTOR_BINDING_OUTPUT_COLOR_TRANSFER];
			uniformFlushStages |= vk::PipelineStageFlagBits::eFragmentShader;
		}

		void writeDescriptorSets() {
			const std::array projectionMatrixBuffers = {
				vk::DescriptorBufferInfo(
					resources->uniformBuffer.getBuffer(),												//Buffer
					uniformBufferLayout[RendererBase::DESCRIPTOR_BINDING_PROJECTION_MATRIX].offset(),	//Offset
					uniformBufferLayout[RendererBase::DESCRIPTOR_BINDING_PROJECTION_MATRIX].size()		//Size
				)
			};
			const std::array colorTransferBuffers = {
				vk::DescriptorBufferInfo(
					resources->uniformBuffer.getBuffer(),											//Buffer
					uniformBufferLayout[RendererBase::DESCRIPTOR_BINDING_OUTPUT_COLOR_TRANSFER].offset(),	//Offset
					uniformBufferLayout[RendererBase::DESCRIPTOR_BINDING_OUTPUT_COLOR_TRANSFER].size()		//Size
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
					RendererBase::DESCRIPTOR_BINDING_OUTPUT_COLOR_TRANSFER,		//Binding
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

		static vk::RenderPass createRenderPass(	const Graphics::Vulkan& vulkan, 
												const Graphics::Frame::Descriptor& frameDesc,
												DepthStencilFormat depthStencilFmt ) 
		{
			return Graphics::Drawtable::getRenderPass(vulkan, frameDesc, depthStencilFmt);
		}

		static Graphics::StagedBuffer createUniformBuffer(	const Graphics::Vulkan& vulkan, 
															const RendererBase::UniformBufferLayout& layout ) 
		{
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

		static Math::Mat4x4f& getProjectionMatrix(	const RendererBase::UniformBufferLayout& uniformBufferLayout,
													Graphics::StagedBuffer& uniformBuffer )
		{
			const auto& area = uniformBufferLayout[RendererBase::DESCRIPTOR_BINDING_PROJECTION_MATRIX];
			return *(reinterpret_cast<Math::Mat4x4f*>(area.begin(uniformBuffer.data())));
		}

		static Utils::BufferView<std::byte> getOutputColorTransfer(	const RendererBase::UniformBufferLayout& uniformBufferLayout,
																	Graphics::StagedBuffer& uniformBuffer )
		{
			const auto& area = uniformBufferLayout[RendererBase::DESCRIPTOR_BINDING_OUTPUT_COLOR_TRANSFER];
			return Utils::BufferView<std::byte>(area.begin(uniformBuffer.data()), area.size());
		}
	};

	using Output = Signal::Output<Video>;

	std::reference_wrapper<Compositor> 			owner;

	Output										videoOut;

	std::unique_ptr<Open>						opened;
	bool										hasChanged;

	CompositorImpl(	Compositor& comp )
		: owner(comp)
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
			if(hasChanged || compositor.layersHaveChanged()) {
				videoOut.push(opened->draw(compositor));

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

}
