/*
 * This example shows how to use the BezierCrop class
 * 
 * How to compile:
 * c++ 01\ -\ Bezier\ Crop.cpp -std=c++17 -Wall -Wextra -lzuazo -lzuazo-window -lzuazo-ffmpeg -lzuazo-compositor -lglfw -ldl -lpthread -lavutil -lavformat -lavcodec -lswscale
 */

#include <zuazo/Instance.h>
#include <zuazo/Player.h>
#include <zuazo/Modules/Window.h>
#include <zuazo/Consumers/WindowRenderer.h>
#include <zuazo/Processors/Layers/BezierCrop.h>
#include <zuazo/Sources/FFmpegClip.h>
#include <zuazo/Math/Geometry.h>

#include <mutex>
#include <iostream>


static Zuazo::Math::CubicBezierLoop<Zuazo::Math::Vec2f> 
createLoop(	Zuazo::Utils::BufferView<const std::array<Zuazo::Math::Vec2f, 3>> points,
			Zuazo::Math::Vec2f& loopSize ) 
{
	//Create a bezier loop with the points
	auto loop = Zuazo::Math::CubicBezierLoop<Zuazo::Math::Vec2f>(
		Zuazo::Utils::BufferView<const std::array<Zuazo::Math::Vec2f, 3>>(points)
	);

	//This is kinda ugly, center the loop by obtaining is center.
	//It also comes handy to obtain the size :-)
	const auto loopBoundaries = Zuazo::Math::getBoundaries(loop);
	const auto loopCenter = (loopBoundaries.getMin() + loopBoundaries.getMax()) / 2;
	loopSize = loopBoundaries.getMax() - loopBoundaries.getMin();

	//Center the points
	std::vector<std::array<Zuazo::Math::Vec2f, 3>> centeredPoints(points.cbegin(), points.cend());
	for(auto& segment : centeredPoints) {
		for(auto& point : segment) {
			point -= loopCenter;
		}
	}

	return Zuazo::Math::CubicBezierLoop<Zuazo::Math::Vec2f>(
		Zuazo::Utils::BufferView<const std::array<Zuazo::Math::Vec2f, 3>>(centeredPoints)
	);
}







int main(int argc, const char* argv[]) {
	if(argc != 2) {
		std::cerr << "Usage: " << *argv << " <video_file>" << std::endl;
		std::terminate();
	}

	//Instantiate Zuazo as usual. Note that we're loading the Window module
	Zuazo::Instance::ApplicationInfo appInfo(
		"Compositor Example 01",					//Application's name
		Zuazo::Version(0, 1, 0),					//Application's version
		Zuazo::Verbosity::GEQ_INFO,					//Verbosity 
		{ Zuazo::Modules::Window::get() }			//Modules
	);
	Zuazo::Instance instance(std::move(appInfo));
	std::unique_lock<Zuazo::Instance> lock(instance);

	//Construct the desired parameters
	const Zuazo::VideoMode videoMode(
		Zuazo::Utils::MustBe<Zuazo::Rate>(Zuazo::Rate(25, 1)), //Just specify the desired rate
		Zuazo::Utils::Any<Zuazo::Resolution>(),
		Zuazo::Utils::Any<Zuazo::AspectRatio>(),
		Zuazo::Utils::Any<Zuazo::ColorPrimaries>(),
		Zuazo::Utils::Any<Zuazo::ColorModel>(),
		Zuazo::Utils::Any<Zuazo::ColorTransferFunction>(),
		Zuazo::Utils::Any<Zuazo::ColorSubsampling>(),
		Zuazo::Utils::Any<Zuazo::ColorRange>(),
		Zuazo::Utils::Any<Zuazo::ColorFormat>()	
	);

	const Zuazo::Utils::Limit<Zuazo::DepthStencilFormat> depthStencil(
		Zuazo::Utils::MustBe<Zuazo::DepthStencilFormat>(Zuazo::DepthStencilFormat::NONE) //Not interested in the depth buffer
	);

	const auto windowSize = Zuazo::Math::Vec2i(1280, 720);

	const auto& monitor = Zuazo::Consumers::WindowRenderer::NO_MONITOR; //Not interested in the full-screen mode

	//Construct the window object
	Zuazo::Consumers::WindowRenderer window(
		instance, 						//Instance
		"Output Window",				//Layout name
		videoMode,						//Video mode limits
		depthStencil,					//Depth buffer limits
		windowSize,						//Window size (in screen coordinates)
		monitor							//Monitor for setting fullscreen
	);

	//Open the window (now becomes visible)
	window.setResizeable(false); //Disable resizeing, as extra care needs to be taken
	window.asyncOpen(lock);

	//Bezier loops
	const std::array<std::array<Zuazo::Math::Vec2f, 3>, 4> HEART_POINTS = {
		5.0f*Zuazo::Math::Vec2f(0,0),
		5.0f*Zuazo::Math::Vec2f(-15,-30),
		5.0f*Zuazo::Math::Vec2f(-40,-30),
		5.0f*Zuazo::Math::Vec2f(-40,-5),
		5.0f*Zuazo::Math::Vec2f(-40,25),
		5.0f*Zuazo::Math::Vec2f(-10,50),
		5.0f*Zuazo::Math::Vec2f(0,50),
		5.0f*Zuazo::Math::Vec2f(10,50),
		5.0f*Zuazo::Math::Vec2f(40,25),
		5.0f*Zuazo::Math::Vec2f(40,-5),
		5.0f*Zuazo::Math::Vec2f(40,-30),
		5.0f*Zuazo::Math::Vec2f(15,-30)
	};

	const std::array<std::array<Zuazo::Math::Vec2f, 3>, 4> BLOB_POINTS = {
		5.0f*Zuazo::Math::Vec2f(0,0),
		5.0f*Zuazo::Math::Vec2f(25,10),
		5.0f*Zuazo::Math::Vec2f(30,-10),
		5.0f*Zuazo::Math::Vec2f(45,0),
		5.0f*Zuazo::Math::Vec2f(60,15),
		5.0f*Zuazo::Math::Vec2f(40,20),
		5.0f*Zuazo::Math::Vec2f(45,30),
		5.0f*Zuazo::Math::Vec2f(60,45),
		5.0f*Zuazo::Math::Vec2f(0,65),
		5.0f*Zuazo::Math::Vec2f(10,50),
		5.0f*Zuazo::Math::Vec2f(20,30),
		5.0f*Zuazo::Math::Vec2f(20,15)
	};

	//Create a bezier loop with the points
	Zuazo::Math::Vec2f loopSize;
	auto loop = createLoop(HEART_POINTS, loopSize);

	//Create a layer for rendering to the window
	Zuazo::Processors::Layers::BezierCrop bezierCrop(
		instance,
		"Video Surface",
		&window,
		loopSize,
		std::move(loop)
	);

	window.setLayers({bezierCrop});
	bezierCrop.setScalingMode(Zuazo::ScalingMode::CROPPED);
	bezierCrop.setScalingFilter(Zuazo::ScalingFilter::NEAREST);
	bezierCrop.setLineWidth(10.0f);
	bezierCrop.setLineColor(Zuazo::Math::Vec4f(1, 0, 1, 1));
	bezierCrop.asyncOpen(lock);

	//Create a video source
	Zuazo::Sources::FFmpegClip videoClip(
		instance,
		"Video Source",
		Zuazo::VideoMode::ANY,
		std::string(argv[1])
	);

	videoClip.play();
	videoClip.setRepeat(Zuazo::ClipBase::Repeat::REPEAT);
	videoClip.asyncOpen(lock);

	//Create a player for playing the clip
	Zuazo::Player clipPlayer(instance, &videoClip);
	clipPlayer.enable();

	//Route the signal
	bezierCrop << videoClip;

	//Show some data about the preferences to the user
	std::cout << "\nSupported video-modes:\n";
	for(const auto& videoMode : window.getVideoModeCompatibility()) {
		std::cout << "\t-" << videoMode << "\n";
	}

	std::cout << "\nSelected video-mode:\n";
	std::cout << "\t-" << window.getVideoMode() << "\n";

	std::cout << "\nSupported depth-stencil formats:\n";
	std::cout << "\t-" << window.getDepthStencilFormatCompatibility() << "\n";

	std::cout << "\nSelected depth-stencil format:\n";
	std::cout << "\t-" << window.getDepthStencilFormat() << "\n";

	//Done!
	lock.unlock();
	getchar();
	lock.lock();
}