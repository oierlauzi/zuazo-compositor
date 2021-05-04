#include <zuazo/Renderers/Compositor.h>

#include <zuazo/LayerBase.h>
#include <zuazo/Graphics/CommandBuffer.h>
#include <zuazo/Graphics/UniformBuffer.h>
#include <zuazo/Graphics/TargetFramePool.h>
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

namespace Zuazo::Renderers {

/*
 * CompositorImpl
 */

struct CompositorImpl {
	struct Open {
		struct Resources {
			Resources(	Graphics::UniformBuffer uniformBuffer,
						vk::UniqueDescriptorPool descriptorPool )
				: uniformBuffer(std::move(uniformBuffer))
				, descriptorPool(std::move(descriptorPool))
			{
			}

			~Resources() = default;

			Graphics::UniformBuffer								uniformBuffer;
			vk::UniqueDescriptorPool							descriptorPool;
		};

		struct Cache {
			std::vector<Compositor::LayerRef>			layers;
		};

		const Graphics::Vulkan& 					vulkan;

		std::shared_ptr<Resources>					resources;
		vk::DescriptorSet							descriptorSet;
		vk::PipelineLayout							pipelineLayout;

		Graphics::TargetFramePool 					framePool;
		Graphics::CommandBufferPool					commandBufferPool;
		
		Utils::BufferView<const vk::ClearValue>		clearValues;

		Open(	const Graphics::Vulkan& vulkan, 
				const Graphics::Frame::Descriptor& frameDesc,
				DepthStencilFormat depthStencilFmt,
				const Compositor::Camera& cam )
			: vulkan(vulkan)
			, resources(Utils::makeShared<Resources>(	createUniformBuffer(vulkan),
														createDescriptorPool(vulkan) ))
			, descriptorSet(createDescriptorSet(vulkan, *(resources->descriptorPool)))
			, pipelineLayout(RendererBase::getBasePipelineLayout(vulkan))

			, framePool(createFramePool(vulkan, frameDesc, depthStencilFmt))
			, commandBufferPool(createCommandBufferPool(vulkan))

			, clearValues(Graphics::RenderPass::getClearValues(depthStencilFmt))
		{
			//Bind the uniform buffers to the descriptor sets
			resources->uniformBuffer.writeDescirptorSet(vulkan, descriptorSet);

			//Update the contents of the uniforms buffers
			updateProjectionMatrixUniform(cam);
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

				MODIFICATION_COUNT
			};

			std::bitset<MODIFICATION_COUNT> modifications;

			modifications.set(RECREATE_DRAWTABLE); //Best guess
			modifications.set(UPDATE_PROJECTION_MATRIX, framePool.getFrameDescriptor().calculateSize() != frameDesc.calculateSize());

			//Evaluate which modifications need to be done
			if(modifications.test(RECREATE_DRAWTABLE)) {
				framePool = createFramePool(framePool.getVulkan(), frameDesc, depthStencilFmt);
				modifications.set(RECREATE_CLEAR_VALUES); //Best guess
			}

			if(modifications.test(RECREATE_CLEAR_VALUES)) {
				clearValues = Graphics::RenderPass::getClearValues(depthStencilFmt);
			}

			if(modifications.test(UPDATE_PROJECTION_MATRIX)) {
				updateProjectionMatrixUniform(cam);
			}
		}

		void setCamera(const Compositor::Camera& cam) {
			updateProjectionMatrixUniform(cam);
		}

		Video draw(RendererBase& renderer) {
			//Obtain the viewports and the scissors
			const auto extent = Graphics::toVulkan(framePool.getFrameDescriptor().getResolution());
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
			auto result = framePool.acquireFrame();
			auto commandBuffer = commandBufferPool.acquireCommandBuffer();

			//Begin the commandbuffer
			constexpr vk::CommandBufferBeginInfo cmdBeginInfo(
				vk::CommandBufferUsageFlagBits::eOneTimeSubmit
			);
			commandBuffer->begin(cmdBeginInfo);

			//Add the compositor related dependencies to it
			commandBuffer->addDependencies({resources});

			//Draw to the command buffer
			result->beginRenderPass(
				commandBuffer->get(),
				scissors.front(),
				clearValues, 
				vk::SubpassContents::eInline
			);

			//Execute all the command buffers gathered from the layers
			if(!renderer.getLayers().empty()) {
				//Flush the uniform buffer, as it will be used
				resources->uniformBuffer.flush(vulkan);

				//Bind all descriptors
				commandBuffer->bindDescriptorSets(
					vk::PipelineBindPoint::eGraphics,								//Pipeline bind point
					pipelineLayout,													//Pipeline layout
					RendererBase::DESCRIPTOR_SET,									//First index
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
			result->endRenderPass(commandBuffer->get());
			commandBuffer->end();

			//Draw to the frame
			result->draw(std::move(commandBuffer));

			return result;
		}

	private:
		void updateProjectionMatrixUniform(const Compositor::Camera& cam) {
			resources->uniformBuffer.waitCompletion(vulkan);

			const auto size = framePool.getFrameDescriptor().calculateSize();
			const auto mtx = cam.calculateMatrix(size);
			resources->uniformBuffer.write(
				vulkan,
				RendererBase::DESCRIPTOR_BINDING_PROJECTION_MATRIX,
				&mtx,
				sizeof(mtx)
			);
		}

		static Graphics::UniformBuffer createUniformBuffer(const Graphics::Vulkan& vulkan) 
		{
			return Graphics::UniformBuffer(vulkan, RendererBase::getUniformBufferSizes());
		}

		static vk::UniqueDescriptorPool createDescriptorPool(const Graphics::Vulkan& vulkan){
			const auto poolSizes = RendererBase::getDescriptorPoolSizes();
			
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

		static Graphics::TargetFramePool createFramePool(	const Graphics::Vulkan& vulkan, 
															const Graphics::Frame::Descriptor& desc,
															DepthStencilFormat depthStencilFmt )
		{
			return Graphics::TargetFramePool(
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

	void open(ZuazoBase& base, std::unique_lock<Instance>* lock = nullptr) {
		auto& compositor = static_cast<Compositor&>(base);
		assert(&owner.get() == &compositor);
		assert(!opened);

		if(static_cast<bool>(compositor.getVideoMode())) {
			//Create in a unlocked environment
			if(lock) lock->unlock();
			auto newOpened = Utils::makeUnique<Open>(
				compositor.getInstance().getVulkan(),
				compositor.getVideoMode().getFrameDescriptor(),
				compositor.getDepthStencilFormat(),
				compositor.getCamera()
			);
			if(lock) lock->lock();

			//Write changes after locking back
			opened = std::move(newOpened);
			compositor.setViewportSize(opened->framePool.getFrameDescriptor().calculateSize());
		}

		hasChanged = true; //Signal rendering if needed
	}

	void asyncOpen(ZuazoBase& base, std::unique_lock<Instance>& lock) {
		assert(lock.owns_lock());
		open(base, &lock);
		assert(lock.owns_lock());
	}

	void close(ZuazoBase& base, std::unique_lock<Instance>* lock = nullptr) {
		auto& compositor = static_cast<Compositor&>(base);
		assert(&owner.get() == &compositor);
		
		//Write changles
		videoOut.reset();
		auto oldOpened = std::move(opened);

		//Reset in a unlocked environment
		if(oldOpened) {
			if(lock) lock->unlock();
			oldOpened.reset();
			if(lock) lock->lock();
		}

		assert(!opened);
	}

	void asyncClose(ZuazoBase& base, std::unique_lock<Instance>& lock) {
		assert(lock.owns_lock());
		close(base, &lock);
		assert(lock.owns_lock());
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
			Utils::MustBe<ColorModel>(ColorModel::RGB),
			Utils::MustBe<ColorTransferFunction>(ColorTransferFunction::LINEAR),
			Utils::MustBe<ColorSubsampling>(ColorSubsampling::RB_444),
			Utils::Any<ColorRange>(),
			Graphics::TargetFrame::getSupportedFormats(compositor.getInstance().getVulkan())
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
			Graphics::TargetFrame::getSupportedSrgbFormats(compositor.getInstance().getVulkan())
		);

		return result;
	}



	void recreateCallback(	Compositor& compositor, 
							const VideoMode& videoMode, 
							DepthStencilFormat depthStencilFormat )
	{
		assert(&owner.get() == &compositor);

		if(compositor.isOpen()) {
			const auto isValid = static_cast<bool>(videoMode);

			if(opened && isValid) {
				//Video mode remains valid
				opened->recreate(
					videoMode.getFrameDescriptor(),
					depthStencilFormat,
					compositor.getCamera()
				);

				//Update the size
				compositor.setViewportSize(opened->framePool.getFrameDescriptor().calculateSize());
			} else if(opened && !isValid) {
				//Video mode is not valid anymore
				opened.reset();
				videoOut.reset();
			} else if(!opened && isValid) {
				//Video mode has become valid
				opened = Utils::makeUnique<Open>(
					compositor.getInstance().getVulkan(),
					videoMode.getFrameDescriptor(),
					depthStencilFormat,
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

	void depthStencilCallback(RendererBase& base, DepthStencilFormat depthStencilFormat) {
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

	const Graphics::RenderPass& renderPassQueryCallback(const RendererBase& base) {
		const auto& compositor = static_cast<const Compositor&>(base);
		assert(&owner.get() == &compositor); 
		static const Graphics::RenderPass NO_RENDER_PASS;

		return opened ? opened->framePool.getRenderPass() : NO_RENDER_PASS;
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
						std::string name )
	: Utils::Pimpl<CompositorImpl>({}, *this)
	, ZuazoBase(
		instance, 
		std::move(name),
		PadRef((*this)->videoOut),
		std::bind(&CompositorImpl::moved, std::ref(**this), std::placeholders::_1),
		std::bind(&CompositorImpl::open, std::ref(**this), std::placeholders::_1, nullptr),
		std::bind(&CompositorImpl::asyncOpen, std::ref(**this), std::placeholders::_1, std::placeholders::_2),
		std::bind(&CompositorImpl::close, std::ref(**this), std::placeholders::_1, nullptr),
		std::bind(&CompositorImpl::asyncClose, std::ref(**this), std::placeholders::_1, std::placeholders::_2),
		std::bind(&CompositorImpl::update, std::ref(**this)) )
	, VideoBase(
		std::bind(&CompositorImpl::videoModeCallback, std::ref(**this), std::placeholders::_1, std::placeholders::_2) )
	, RendererBase(
		std::bind(&CompositorImpl::depthStencilCallback, std::ref(**this), std::placeholders::_1, std::placeholders::_2),
		std::bind(&CompositorImpl::cameraCallback, std::ref(**this), std::placeholders::_1, std::placeholders::_2),
		std::bind(&CompositorImpl::renderPassQueryCallback, std::ref(**this), std::placeholders::_1) )
	, Signal::SourceLayout<Video>(makeProxy((*this)->videoOut))
{
	setVideoModeCompatibility((*this)->getVideoModeCompatibility());
}

Compositor::Compositor(Compositor&& other) = default;

Compositor::~Compositor() = default;

Compositor& Compositor::operator=(Compositor&& other) = default;

}
