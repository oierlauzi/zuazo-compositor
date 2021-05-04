/*
 * This example shows how to render to a off-screen renderer
 * 
 * How to compile:
 * c++ 00\ -\ Instantiating.cpp -std=c++17 -Wall -Wextra -lzuazo -lzuazo-window -lzuazo-ffmpeg -lzuazo-compositor -lglfw -ldl -lpthread -lavutil -lavformat -lavcodec -lswscale
 */


#include <zuazo/Instance.h>
#include <zuazo/Player.h>
#include <zuazo/Modules/Window.h>
#include <zuazo/Renderers/Window.h>
#include <zuazo/Renderers/Compositor.h>
#include <zuazo/Layers/VideoSurface.h>
#include <zuazo/Sources/FFmpegClip.h>
#include <zuazo/Consumers/RendererWrapper.h>

#include <mutex>
#include <iostream>



static Zuazo::Math::Vec2f getRandomVec2f() {
	return Zuazo::Math::Vec2f(
		static_cast<float>(rand()) / static_cast<float>(RAND_MAX),
		static_cast<float>(rand()) / static_cast<float>(RAND_MAX)
	);
}



int main(int argc, const char** argv) {
	if(argc != 2) {
		std::cerr << "Usage: " << *argv << " <video file>" << std::endl;
		std::terminate();
	}

	//Instantiate Zuazo as usual. Note that we're loading the Window module
	Zuazo::Instance::ApplicationInfo appInfo(
		"Compositor Example 00",					//Application's name
		Zuazo::Version(0, 1, 0),					//Application's version
		Zuazo::Verbosity::GEQ_INFO,					//Verbosity 
		{ Zuazo::Modules::Window::get() }			//Modules
	);
	Zuazo::Instance instance(std::move(appInfo));
	std::unique_lock<Zuazo::Instance> lock(instance);



	//Configure the output window
	const auto windowSize = Zuazo::Math::Vec2i(1280, 720);
	const auto& monitor = Zuazo::Renderers::Window::NO_MONITOR; //Not interested in the full-screen mode

	Zuazo::Consumers::RendererWrapper<Zuazo::Renderers::Window> window(
		instance,
		"Output Window",
		windowSize,
		monitor
	);

	window.setVideoModeNegotiationCallback(
		[] (Zuazo::VideoBase&, const std::vector<Zuazo::VideoMode>& compatibility) -> Zuazo::VideoMode {
			auto result = compatibility.front();
			result.setFrameRate(Zuazo::Utils::MustBe<Zuazo::Rate>(result.getFrameRate().highest()));
			return result;
		}
	);

	window.asyncOpen(lock);



	//Create the input clip
	Zuazo::Sources::FFmpegClip videoClip(
		instance,
		"Input Video",
		std::string(argv[1])
	);
	videoClip.setRepeat(Zuazo::Sources::FFmpegClip::Repeat::REPEAT);
	videoClip.play();
	videoClip.asyncOpen(lock);

	Zuazo::Player videoClipPlayer(instance, &videoClip);
	videoClipPlayer.enable();



	//Create the compositor and the layer vector
	std::vector<Zuazo::Layers::VideoSurface> layers;
	Zuazo::Renderers::Compositor compositor(
		instance,
		"Compositor"
	);

	compositor.setVideoModeNegotiationCallback(
		[] (Zuazo::VideoBase&, const std::vector<Zuazo::VideoMode>&) -> Zuazo::VideoMode {
			return Zuazo::VideoMode(
				Zuazo::Utils::Any<Zuazo::Rate>(),
				Zuazo::Utils::MustBe<Zuazo::Resolution>(Zuazo::Resolution(1280, 720)),
				Zuazo::Utils::MustBe<Zuazo::AspectRatio>(Zuazo::AspectRatio(1, 1)),
				Zuazo::Utils::MustBe<Zuazo::ColorPrimaries>(Zuazo::ColorPrimaries::BT709),
				Zuazo::Utils::MustBe<Zuazo::ColorModel>(Zuazo::ColorModel::RGB),
				Zuazo::Utils::MustBe<Zuazo::ColorTransferFunction>(Zuazo::ColorTransferFunction::LINEAR),
				Zuazo::Utils::MustBe<Zuazo::ColorSubsampling>(Zuazo::ColorSubsampling::RB_444),
				Zuazo::Utils::MustBe<Zuazo::ColorRange>(Zuazo::ColorRange::FULL),
				Zuazo::Utils::MustBe<Zuazo::ColorFormat>(Zuazo::ColorFormat::R16fG16fB16fA16f)
			);
		}
	);

	compositor.asyncOpen(lock);

	//Configure keyboard callback for the window
	const auto keyCallback = [&compositor, &videoClip, &layers] (	Zuazo::Renderers::Window&, 
																	Zuazo::KeyboardKey key, 
																	Zuazo::KeyEvent event, 
																	Zuazo::KeyModifiers)
	{
		if(event == Zuazo::KeyEvent::PRESS) {
			//We only care for presses
			switch(key) {
			case Zuazo::KeyboardKey::SPACE:
			case Zuazo::KeyboardKey::ENTER:
				std::cout << "Adding layer #" << compositor.getLayers().size() << std::endl;
				{
					const auto size = static_cast<Zuazo::Math::Vec2f>(compositor.getVideoMode().getResolutionValue());

					layers.emplace_back(
						compositor.getInstance(),
						"Compositor Layer" + Zuazo::toString(compositor.getLayers().size()),
						&compositor,
						getRandomVec2f() * size / 2.0f
					);
					auto transform = layers.back().getTransform();
					transform.setPosition(Zuazo::Math::Vec3f(Zuazo::Math::lerp(-size/2.0f, size/2.0f, getRandomVec2f()), 0.0f));
					layers.back().setTransform(transform);
					layers.back().open();
					layers.back() << videoClip;

					const std::vector<Zuazo::Renderers::Compositor::LayerRef> newLayers(
						layers.cbegin(), layers.cend()
					);
					compositor.setLayers(newLayers);
				}
				
				break;

			case Zuazo::KeyboardKey::BACKSPACE:
			case Zuazo::KeyboardKey::DELETE:
				if(layers.size()) {
					layers.pop_back();
					std::cout << "Removing a layer #" << compositor.getLayers().size() << std::endl;

					const std::vector<Zuazo::Renderers::Compositor::LayerRef> newLayers(
						layers.cbegin(), layers.cend()
					);
					compositor.setLayers(newLayers);
				}

				break;

			default:
				//Unknown action
				break;
			}
		}
	};
	window.getRenderer().setKeyboardCallback(keyCallback);

	std::cout << "Compositor's video-mode:" << std::endl;
	std::cout << compositor.getVideoMode() << std::endl;

	//Route the signals
	window << compositor;

	//Done!
	lock.unlock();
	getchar();
	lock.lock();
}
