#include <zuazo/Processors/Compositor.h>

#include <zuazo/LayerData.h>
#include <zuazo/Graphics/CommandBuffer.h>
#include <zuazo/Graphics/Drawtable.h>
#include <zuazo/Signal/Input.h>
#include <zuazo/Signal/Output.h>
#include <zuazo/Utils/Pool.h>
#include <zuazo/Utils/StaticId.h>


#include <memory>
#include <vector>
#include <utility>
#include <algorithm>
#include <tuple>

namespace Zuazo::Processors {

/*
 * CompositorImpl
 */

struct CompositorImpl {
	struct Open {
		using CommandPool = Utils::Pool<Graphics::CommandBuffer>;
		using Layers = std::vector<std::reference_wrapper<const LayerData>>;

		struct Cache {
			std::vector<const vk::CommandBuffer>		drawCommandBuffers;
			std::vector<std::shared_ptr<const void>>	dependencies;
		};

		std::shared_ptr<vk::UniqueRenderPass>		renderPass;
		Graphics::Drawtable 						drawtable;
		CommandPool									commandBufferPool;
		Layers										layers;
		Cache										cache;

		Open(	const Graphics::Vulkan& vulkan, 
				const Graphics::Frame::Descriptor& conf,
				vk::Format depthStencilFmt)
			: renderPass(createRenderPass(vulkan, conf))
			, drawtable(vulkan, conf, renderPass, depthStencilFmt)
			, commandBufferPool() //TODO
		{
		}

		~Open() = default;

		void recreate(	const Graphics::Frame::Descriptor& conf,
						vk::Format depthStencilFmt )
		{
			renderPass = createRenderPass(drawtable.getVulkan(), conf); 
			drawtable = Graphics::Drawtable(drawtable.getVulkan(), conf, renderPass, depthStencilFmt);
		}

		void addLayer(const LayerData& layer) {
			layers.push_back(layer);
		}

		Video draw() {
			//Obtain a new frame and command buffer
			auto result = drawtable.acquireFrame();
			auto commandBuffer = commandBufferPool.acquire();

			//Sort the layers based on their alpha and depth. Stable sort is used to preserve order
			std::stable_sort(
				layers.begin(), layers.end(),
				std::bind(&Open::layerComp, *this, std::placeholders::_1, std::placeholders::_2)
			);

			//Obtain all the vulkan command buffers and add them as a dependency
			for(const auto& layer : layers) {
				const auto& commandBuffer = layer.get().getCommandBuffer();
				cache.drawCommandBuffers.push_back(commandBuffer->getCommandBuffer());
				cache.dependencies.push_back(commandBuffer);
			}

			//Draw to the command buffer
			//result->beginRenderPass(commandBuffer->getCommandBuffer(), /*TODO*/);
			commandBuffer->execute(cache.drawCommandBuffers); //Execute all the command buffers gathered from the layers
			result->endRenderPass(commandBuffer->getCommandBuffer());

			//Add dependencies to the command buffer
			commandBuffer->setDependencies(cache.dependencies);

			//Draw to the frame
			result->draw(std::move(commandBuffer));

			//Clear the state. This should not deallocate them
			layers.clear(); 
			cache.drawCommandBuffers.clear();
			cache.dependencies.clear();

			return result;
		}

	private:
		bool layerComp(const LayerData& a, const LayerData& b) {
			/*
			 * The strategy will be the following:
			 * 1. Draw the BACKGROUND from bottom to top
			 * 2. Draw the alphaless SCENE backwards, writing and testing depth
			 * 3. Draw the transparent SCENE forwards, writing and testing depth
			 * 4. Draw the FOREGROUND from bottom to top
             */
			
			if(	a.getRenderingStage() == Compositor::RenderingStage::SCENE &&
				b.getRenderingStage() == Compositor::RenderingStage::SCENE ) 
			{
				//Both are in the scene stage so transparency must be taken into consideration
				assert(a.getRenderingStage() == b.getRenderingStage());

				if(a.getHasAlpha() != b.getHasAlpha()) {
					//Prioritize alphaless drawing
					return a.getHasAlpha() < b.getHasAlpha();
				} else {
					//Both or neither have alpha. Depth must be taken in consideration
					assert(a.getHasAlpha() == b.getHasAlpha());
					const auto hasAlpha = a.getHasAlpha();

					//Calculate the average depth
					const Math::Mat4x4f& projMatrix = Math::Mat4x4f();
					const auto aDepth = (projMatrix * a.getAveragePosition()).z;
					const auto bDepth = (projMatrix * b.getAveragePosition()).z;

					return !hasAlpha
						? aDepth < bDepth
						: aDepth > bDepth;
				}
			} else {
				//Just compare Rendering stages
				return a.getRenderingStage() < b.getRenderingStage();
			}
		}

		bool layerCompStencil(const LayerData& a, const LayerData& b) {
			/*
			 * The strategy will be the following:
			 * 1. Draw the alphaless FOREGROUND from top to bottom, writing (0x02) and testing (GEQ) stencil
			 * 2. Draw the alphaless SCENE backwards, writing and testing depth, writing (0x01) and testing (GEQ) stencil
			 * 3. Draw the alphaless BACKGROUND from top to bottom, writing (0x00) and testing (GEQ) stencil
			 * 4. Draw the transparent BACKGROUND from bottom to top, writing (0x00) and testing (GEQ) stencil
			 * 5. Draw the transparent SCENE forwards, writing and testing depth, writing (0x01) and testing (GEQ) stencil
			 * 6. Draw the transparent FOREGROUND from top to bottom, writing (0x02) and testing (GEQ) stencil
			 * 
			 * //FIXME this technique wont preserve background and foreground layer ordering. Somehow layer # should be written
			 * to the depth buffer so that the ordering is preserved. Depth buffer should be cleared from stage to stage.
             */

			assert(false); //TODO
		}

		static std::shared_ptr<vk::UniqueRenderPass> createRenderPass(	const Graphics::Vulkan& vulkan, 
																		const Graphics::Frame::Descriptor& conf ) 
		{

		}

	};

	using LayerInput = Signal::Input<LayerDataStream>;
	using Output = Signal::Output<Video>;

	std::reference_wrapper<Compositor> 			owner;

	vk::Format									depthStencilFormat;
	Compositor::Camera							camera;

	std::vector<LayerInput>						layerIns;
	Output										videoOut;

	std::unique_ptr<Open>						opened;
	bool										hasChanged;

	CompositorImpl(Compositor& comp) 
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

		opened = Utils::makeUnique<Open>(); //TODO
		hasChanged = true; //Signal rendering if needed
	}

	void close(ZuazoBase& base) {
		assert(opened);
		auto& compositor = static_cast<Compositor&>(base);
		assert(&owner.get() == &compositor);
        
        videoOut.reset();
		opened.reset();
	}

	void update() {
		auto& compositor = owner.get();

		const bool inputsHaveChanged = std::any_of(
			layerIns.cbegin(), layerIns.cend(),
			[] (const LayerInput& input) -> bool {
				return input.hasChanged();
			}
		);

		if(opened && (hasChanged || inputsHaveChanged)) {
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

	void videoModeCallback(VideoBase& base, const VideoMode& videoMode) {
		auto& compositor = static_cast<Compositor&>(base);
		assert(&owner.get() == &compositor);

		if(opened) {
			opened->recreate(
				videoMode ? videoMode.getFrameDescriptor() : Graphics::Frame::Descriptor(),
				depthStencilFormat
			);
		}

		hasChanged = true;
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

	void setCamera(const Compositor::Camera& cam) {
		camera = cam;
		hasChanged = true;
	}

	const Compositor::Camera& getCamera() const {
		return camera;
	}



	static vk::RenderPass createRenderPass(	const Graphics::Vulkan& vulkan, 
											const Compositor::FrameBufferFormat& fbFormat )
	{
		static std::unordered_map<size_t, Utils::StaticId> ids;
		const size_t framebufferFormatIndex = 
			static_cast<size_t>(fbFormat.colorFormat) << (sizeof(size_t) / 2 * Utils::getByteSize()) | 
			static_cast<size_t>(fbFormat.depthStencilFormat); 
		const auto id = ids[framebufferFormatIndex].get();

		auto result = vulkan.createRenderPass(id);
		if(!result) {
			//Render pass was not created
			const std::array attachments = {
				vk::AttachmentDescription(
					{},												//Flags
					format,											//Attachemnt format
					vk::SampleCountFlagBits::e1,					//Sample count
					vk::AttachmentLoadOp::eClear,					//Color attachment load operation
					vk::AttachmentStoreOp::eStore,					//Color attachemnt store operation
					vk::AttachmentLoadOp::eDontCare,				//Stencil attachment load operation
					vk::AttachmentStoreOp::eDontCare,				//Stencil attachment store operation
					vk::ImageLayout::eUndefined,					//Initial layout
					vk::ImageLayout::ePresentSrcKHR					//Final layout
				)
			};

			constexpr std::array attachmentReferences = {
				vk::AttachmentReference(
					0, 												//Attachments index
					vk::ImageLayout::eColorAttachmentOptimal 		//Attachemnt layout
				)
			};

			const std::array subpasses = {
				vk::SubpassDescription(
					{},												//Flags
					vk::PipelineBindPoint::eGraphics,				//Pipeline bind point
					0, nullptr,										//Input attachments
					attachmentReferences.size(), attachmentReferences.data(), //Color attachments
					nullptr,										//Resolve attachemnts
					nullptr,										//Depth / Stencil attachemnts
					0, nullptr										//Preserve attachments
				)
			};

			constexpr std::array subpassDependencies = {
				vk::SubpassDependency(
					VK_SUBPASS_EXTERNAL,							//Source subpass
					0,												//Destination subpass
					vk::PipelineStageFlagBits::eColorAttachmentOutput,//Source stage
					vk::PipelineStageFlagBits::eColorAttachmentOutput,//Destination stage
					{},												//Source access mask
					vk::AccessFlagBits::eColorAttachmentRead | 		//Destintation access mask
						vk::AccessFlagBits::eColorAttachmentWrite
				)
			};

			const vk::RenderPassCreateInfo createInfo(
				{},													//Flags
				attachments.size(), attachments.data(),				//Attachemnts
				subpasses.size(), subpasses.data(),					//Subpasses
				subpassDependencies.size(), subpassDependencies.data()//Subpass dependencies
			);

			result = vulkan.createRenderPass(id, createInfo);
		}

		assert(result);
		return result;
	}

private:
	Output::PullCallback createPullCallback(CompositorImpl* impl) {
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
						VideoMode videoMode )
	: Utils::Pimpl<CompositorImpl>({}, *this)
	, ZuazoBase(instance, 
				std::move(name),
				PadRef((*this)->videoOut),
				std::bind(&CompositorImpl::moved, std::ref(**this), std::placeholders::_1),
				std::bind(&CompositorImpl::open, std::ref(**this), std::placeholders::_1),
				std::bind(&CompositorImpl::close, std::ref(**this), std::placeholders::_1),
				std::bind(&CompositorImpl::update, std::ref(**this)) )
	, VideoBase(
		std::move(videoMode),
		std::bind(&CompositorImpl::videoModeCallback, std::ref(**this), std::placeholders::_1, std::placeholders::_2) )
	, Signal::SourceLayout<Video>(makeProxy((*this)->videoOut))
{

}

Compositor::Compositor(Compositor&& other) = default;

Compositor::~Compositor() = default;

Compositor& Compositor::operator=(Compositor&& other) = default;


void Compositor::setLayerCount(size_t count) {
	(*this)->setLayerCount(count);
}

size_t Compositor::getLayerCount() const {
	(*this)->getLayerCount();
}


void Compositor::setCamera(const Camera& cam) {
	(*this)->setCamera(cam);
}

const Compositor::Camera& Compositor::getCamera() const {
	(*this)->getCamera();
}


}
