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

#include <zuazo/Math/LoopBlinn/Classifier.h>
#include <zuazo/Math/LoopBlinn/KLMCalculator.h>

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

	const std::array<std::array<Zuazo::Math::Vec2f, 3>, 4> ARC_POINTS = {
		5.0f*Zuazo::Math::Vec2f(0,0),
		5.0f*Zuazo::Math::Vec2f(50,-60),
		5.0f*Zuazo::Math::Vec2f(70,-60),
		5.0f*Zuazo::Math::Vec2f(120,0),
		5.0f*Zuazo::Math::Vec2f(120,5),
		5.0f*Zuazo::Math::Vec2f(120,15),
		5.0f*Zuazo::Math::Vec2f(120,20),
		5.0f*Zuazo::Math::Vec2f(70,-50),
		5.0f*Zuazo::Math::Vec2f(50,-50),
		5.0f*Zuazo::Math::Vec2f(0,20),
		5.0f*Zuazo::Math::Vec2f(0,15),
		5.0f*Zuazo::Math::Vec2f(0,5)
	};

	const std::array<std::array<Zuazo::Math::Vec2f, 3>, 20> PSI_POINTS = {
		Zuazo::Math::Vec2f(-78,-176),
		Zuazo::Math::Vec2f(-78,-172.66667),
		Zuazo::Math::Vec2f(-78,-169.33333),
		Zuazo::Math::Vec2f(-78,-166),
		Zuazo::Math::Vec2f(-28,-166),
		Zuazo::Math::Vec2f(-28,-166),
		Zuazo::Math::Vec2f(-28,-116),
		Zuazo::Math::Vec2f(-28,-73.333333),
		Zuazo::Math::Vec2f(-28,-30.666667),
		Zuazo::Math::Vec2f(-28,12),
		Zuazo::Math::Vec2f(-183,12),
		Zuazo::Math::Vec2f(-48,-178),
		Zuazo::Math::Vec2f(-193,-176),
		Zuazo::Math::Vec2f(-193,-172.66667),
		Zuazo::Math::Vec2f(-193,-169.33333),
		Zuazo::Math::Vec2f(-193,-166),
		Zuazo::Math::Vec2f(-123,-166),
		Zuazo::Math::Vec2f(-243,34),
		Zuazo::Math::Vec2f(-28,34),
		Zuazo::Math::Vec2f(-28,64),
		Zuazo::Math::Vec2f(-28,94),
		Zuazo::Math::Vec2f(-28,124),
		Zuazo::Math::Vec2f(-28,159),
		Zuazo::Math::Vec2f(-28,170),
		Zuazo::Math::Vec2f(-78,170),
		Zuazo::Math::Vec2f(-78,173.33333),
		Zuazo::Math::Vec2f(-78,176.66667),
		Zuazo::Math::Vec2f(-78,180),
		Zuazo::Math::Vec2f(-26,180),
		Zuazo::Math::Vec2f(26,180),
		Zuazo::Math::Vec2f(78,180),
		Zuazo::Math::Vec2f(78,176.66667),
		Zuazo::Math::Vec2f(78,173.33333),
		Zuazo::Math::Vec2f(78,170),
		Zuazo::Math::Vec2f(28,170),
		Zuazo::Math::Vec2f(28,159),
		Zuazo::Math::Vec2f(28,124),
		Zuazo::Math::Vec2f(28,94),
		Zuazo::Math::Vec2f(28,64),
		Zuazo::Math::Vec2f(28,34),
		Zuazo::Math::Vec2f(243,34),
		Zuazo::Math::Vec2f(123,-166),
		Zuazo::Math::Vec2f(193,-166),
		Zuazo::Math::Vec2f(193,-169.33333),
		Zuazo::Math::Vec2f(193,-172.66667),
		Zuazo::Math::Vec2f(193,-176),
		Zuazo::Math::Vec2f(48,-178),
		Zuazo::Math::Vec2f(183,12),
		Zuazo::Math::Vec2f(28,12),
		Zuazo::Math::Vec2f(28,-30.666667),
		Zuazo::Math::Vec2f(28,-73.333333),
		Zuazo::Math::Vec2f(28,-116),
		Zuazo::Math::Vec2f(28,-166),
		Zuazo::Math::Vec2f(28,-166),
		Zuazo::Math::Vec2f(78,-166),
		Zuazo::Math::Vec2f(78,-169.33333),
		Zuazo::Math::Vec2f(78,-172.66667),
		Zuazo::Math::Vec2f(78,-176),
		Zuazo::Math::Vec2f(26,-176),
		Zuazo::Math::Vec2f(-26,-176),
	};

	const std::array<std::array<Zuazo::Math::Vec2f, 3>, 10> BLOB_POINTS = {
		2.0f*Zuazo::Math::Vec2f(0.01776377,-0.17398693),
		2.0f*Zuazo::Math::Vec2f(47.99201,14.161736),
		2.0f*Zuazo::Math::Vec2f(48.965598,-32.077492),
		2.0f*Zuazo::Math::Vec2f(83.348064,-0.01126961),
		2.0f*Zuazo::Math::Vec2f(97.819182,-29.087447),
		2.0f*Zuazo::Math::Vec2f(123.05974,20.423792),
		2.0f*Zuazo::Math::Vec2f(131.82314,0.30587143),
		2.0f*Zuazo::Math::Vec2f(140.49168,-19.594264),
		2.0f*Zuazo::Math::Vec2f(202.56729,29.946176),
		2.0f*Zuazo::Math::Vec2f(144.12213,64.805866),
		2.0f*Zuazo::Math::Vec2f(110.86154,84.644185),
		2.0f*Zuazo::Math::Vec2f(111.93258,45.117509),
		2.0f*Zuazo::Math::Vec2f(96.304268,52.553467),
		2.0f*Zuazo::Math::Vec2f(79.837563,60.388331),
		2.0f*Zuazo::Math::Vec2f(75.96078,62.26659),
		2.0f*Zuazo::Math::Vec2f(58.846595,70.487817),
		2.0f*Zuazo::Math::Vec2f(57.143571,91.471028),
		2.0f*Zuazo::Math::Vec2f(54.613421,79.011568),
		2.0f*Zuazo::Math::Vec2f(39.027068,70.51436),
		2.0f*Zuazo::Math::Vec2f(37.397078,49.030138),
		2.0f*Zuazo::Math::Vec2f(3.2627725,79.797641),
		2.0f*Zuazo::Math::Vec2f(13.270392,82.324973),
		2.0f*Zuazo::Math::Vec2f(13.741668,120.80742),
		2.0f*Zuazo::Math::Vec2f(10.339912,96.872645),
		2.0f*Zuazo::Math::Vec2f(-0.12088594,105.23169),
		2.0f*Zuazo::Math::Vec2f(-18.810885,104.25683),
		2.0f*Zuazo::Math::Vec2f(-40.748849,48.195014),
		2.0f*Zuazo::Math::Vec2f(-35.464911,27.095026),
		2.0f*Zuazo::Math::Vec2f(-30.180973,5.9950382),
		2.0f*Zuazo::Math::Vec2f(-14.484154,-4.7223232),
		/*2.0f*Zuazo::Math::Vec2f(0.01776377,-0.17398693),
		2.0f*Zuazo::Math::Vec2f(47.99201,14.161736),
		2.0f*Zuazo::Math::Vec2f(48.965598,-32.077492),
		2.0f*Zuazo::Math::Vec2f(83.348064,-0.01126961),
		2.0f*Zuazo::Math::Vec2f(97.819182,-29.087447),
		2.0f*Zuazo::Math::Vec2f(105.15965,17.747062),
		2.0f*Zuazo::Math::Vec2f(131.82314,0.30587143),
		2.0f*Zuazo::Math::Vec2f(110.28313,38.492959),
		2.0f*Zuazo::Math::Vec2f(108.76577,22.318506),
		2.0f*Zuazo::Math::Vec2f(144.12213,64.805866),
		2.0f*Zuazo::Math::Vec2f(111.93321,45.118503 ),
		2.0f*Zuazo::Math::Vec2f(111.93321,45.118503),
		2.0f*Zuazo::Math::Vec2f(96.304268,52.553467),
		2.0f*Zuazo::Math::Vec2f(79.837563,60.388331),
		2.0f*Zuazo::Math::Vec2f(75.96078,62.26659),
		2.0f*Zuazo::Math::Vec2f(58.846595,70.487817),
		2.0f*Zuazo::Math::Vec2f(57.143571,91.471028),
		2.0f*Zuazo::Math::Vec2f(54.613421,79.011568),
		2.0f*Zuazo::Math::Vec2f(39.027068,70.51436),
		2.0f*Zuazo::Math::Vec2f(29.160143,-9.0405203),
		2.0f*Zuazo::Math::Vec2f(-40.246104,14.206862),
		2.0f*Zuazo::Math::Vec2f(13.270392,82.324973),
		2.0f*Zuazo::Math::Vec2f(13.741668,120.80742),
		2.0f*Zuazo::Math::Vec2f(10.339912,96.872645),
		2.0f*Zuazo::Math::Vec2f(-0.12088594,105.23169),
		2.0f*Zuazo::Math::Vec2f(-18.810885,104.25683),
		2.0f*Zuazo::Math::Vec2f(-40.748849,48.195014),
		2.0f*Zuazo::Math::Vec2f(-35.464911,27.095026),
		2.0f*Zuazo::Math::Vec2f(-30.180973,5.9950382),
		2.0f*Zuazo::Math::Vec2f(-14.484154,-4.7223232),*/
	};

	//Create a bezier loop with the points
	Zuazo::Math::Vec2f loopSize;
	auto loop = createLoop(PSI_POINTS, loopSize);

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
	//bezierCrop.setLineWidth(5.0f);
	//bezierCrop.setLineSmoothness(4.0f);
	bezierCrop.setLineColor(Zuazo::Math::Vec4f(1, 0, 1, 1));
	bezierCrop.setOpacity(0.5f);
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