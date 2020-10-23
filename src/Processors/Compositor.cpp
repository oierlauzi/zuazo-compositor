#include <zuazo/Processors/Compositor.h>

#include <zuazo/Graphics/CommandBuffer.h>
#include <zuazo/Graphics/Drawtable.h>
#include <zuazo/Signal/Input.h>
#include <zuazo/Signal/Output.h>
#include <zuazo/Utils/Pool.h>

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
				std::bind(&Open::layerDepthComp, *this, std::placeholders::_1, std::placeholders::_2)
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
		bool layerDepthComp(const LayerData& a, const LayerData& b) {
			/*
			 * This function will be called to sort the layers with std::sort(). Therefore, if it returns true
			 * A will be placed before B.
			 * 
			 * The strategy will be the following: Opaque layers should be drawn backwards, in order to take advantage
			 * of the Z-buffer test. Transparent layers should be drawn forwards, so that OIT issues are avoided. Opaque 
			 * layers should be drawn before transparent ones.
			 */
			constexpr auto TRANSPARENT_STAGE = LayerData::RenderingStage::TRANSPARENT;

			const auto& projMatrix = Math::Mat4x4f(); //TODO
			auto depthA = (projMatrix * a.getAveragePosition()).z;
			auto depthB = (projMatrix * b.getAveragePosition()).z;

			//Swap depths if transparent rendering
			if(a.getRenderingStage() == TRANSPARENT_STAGE && b.getRenderingStage() == TRANSPARENT_STAGE) {
				std::swap(depthA, depthB);
			}

			return std::forward_as_tuple(a.getRenderingStage(), depthA) < std::forward_as_tuple(b.getRenderingStage(), depthB);
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

		hasChanged = true;
		update(); //Ensure that a frame is rendered
	}

	void close(ZuazoBase& base) {
		assert(opened);
		auto& compositor = static_cast<Compositor&>(base);
		assert(&owner.get() == &compositor);
        
        videoOut.reset();

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
				{ (*this)->videoOut },
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

}
