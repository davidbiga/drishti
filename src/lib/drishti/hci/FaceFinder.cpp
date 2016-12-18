/*!
  @file   drishti/hci/FaceFinder.cpp
  @author David Hirvonen
  @brief  Face detection and tracking class with GPU acceleration.

  \copyright Copyright 2014-2016 Elucideye, Inc. All rights reserved.
  \license{This project is released under the 3 Clause BSD License.}

*/

#include "drishti/hci/FaceFinder.h"
#include "drishti/hci/gpu/FacePainter.h"
#include "drishti/hci/gpu/FlashFilter.h"

#include "drishti/face/gpu/EyeFilter.h"
#include "drishti/eye/gpu/EllipsoPolarWarp.h"
#include "drishti/eye/gpu/EyeWarp.h"
#include "drishti/core/Logger.h"
#include "drishti/core/drishti_operators.h"
#include "drishti/face/FaceDetectorAndTracker.h"
#include "drishti/face/FaceModelEstimator.h"
#include "drishti/face/FaceDetector.h"
#include "drishti/geometry/motion.h"

#include "ogles_gpgpu/common/proc/fifo.h"

#include <vector>

#define DRISHTI_FACEFILTER_LANDMARKS_WIDTH 1024

#define DRISHTI_FACEFILTER_FLOW_WIDTH 256
#define DRISHTI_FACEFILTER_DO_FLOW_QUIVER 0 // *** display ***
#define DRISHTI_FACEFILTER_DO_CORNER_PLOT 1 // *** display ***

#define DRISHTI_FACEFILTER_FLASH_WIDTH 128

#define DRISHTI_FACEFILTER_DETECTION_WIDTH 512
#define DRISHTI_FACEFILTER_DO_TRACKING 1
#define DRISHTI_FACEFILTER_DO_ACF_MODIFY 0

static const char * sBar = "#################################################################";

// === utility ===

using drishti::face::operator*;
using drishti::core::operator*;

DRISHTI_HCI_NAMESPACE_BEGIN

static int getDetectionImageWidth(float, float, float, float, float);

#if DRISHTI_FACEFILTER_DO_FLOW_QUIVER || DRISHTI_FACEFILTER_DO_CORNER_PLOT
static cv::Size uprightSize(const cv::Size &size, int orientation);
static void extractFlow(const cv::Mat4b &ayxb, const cv::Size &frameSize, ScenePrimitives &scene, float flowScale = 1.f);
static void extractCorners(const cv::Mat1b &corners, ScenePrimitives &scene, float flowScale = 1.f);
#endif

#define DRISHTI_HCI_DEBUG_PYRAMIDS 1

#if DRISHTI_HCI_DEBUG_PYRAMIDS
static void logPyramid(const std::string &filename, const drishti::acf::Detector::Pyramid &P)
{
    cv::Mat Pgpu;
    cv::vconcat(P.data[0][0].get(), Pgpu);
    Pgpu = Pgpu.t();
    cv::normalize(Pgpu, Pgpu, 0, 255, cv::NORM_MINMAX, CV_8UC1);
    cv::imwrite(filename, Pgpu);
}
#endif // DRISHTI_HCI_DEBUG_PYRAMIDS

FaceFinder::FaceFinder(std::shared_ptr<drishti::face::FaceDetectorFactory> &factory, Config &args, void *glContext)
    : m_glContext(glContext)
    , m_hasInit(false)
    , m_outputOrientation(args.outputOrientation)

    // ######## DO Landmarks #########
    , m_doLandmarks(args.doLandmarks)
    , m_landmarksWidth(DRISHTI_FACEFILTER_LANDMARKS_WIDTH)

    // ######## DO FLOW #########
    , m_doFlow(args.doFlow)
    , m_flowWidth(DRISHTI_FACEFILTER_FLOW_WIDTH)

    // ###### DO FLASH ##########
    , m_doFlash(args.doFlash)
    , m_flashWidth(DRISHTI_FACEFILTER_FLASH_WIDTH)

    , m_doIris(DRISHTI_FACEFILTER_DO_ELLIPSO_POLAR)

    , m_scenes(args.frameDelay+1)
    , m_factory(factory)
    , m_sensor(args.sensor)
    , m_logger(args.logger)
    , m_threads(args.threads)
{
    
}

void FaceFinder::setMinDistance(float meters)
{
    m_minDistanceMeters = meters;
}

float FaceFinder::getMinDistance() const
{
    return m_minDistanceMeters;
}

void FaceFinder::setMaxDistance(float meters)
{
    m_maxDistanceMeters = meters;
}

float FaceFinder::getMaxDistance() const
{
    return m_maxDistanceMeters;
}

void FaceFinder::setDoCpuAcf(bool flag)
{
    m_doCpuACF = flag;
}

bool FaceFinder::getDoCpuAcf() const
{
    return m_doCpuACF;
}

void FaceFinder::setFaceFinderInterval(double interval)
{
    m_faceFinderInterval = interval;
}

double FaceFinder::getFaceFinderInterval() const
{
    return m_faceFinderInterval;
}

void FaceFinder::registerFaceMonitorCallback(FaceMonitor *callback)
{
    m_faceMonitorCallback.push_back(callback);
}

void FaceFinder::dump(std::vector<cv::Mat4b> &frames)
{
    if(m_fifo->getBufferCount() == m_fifo->getProcPasses().size())
    {
        auto &filters = m_fifo->getProcPasses();
        frames.resize(filters.size());
        for(int i = 0; i < filters.size(); i++)
        {
            frames[i].create( filters[i]->getOutFrameH(), filters[i]->getOutFrameW() );
            filters[i]->getResultData(frames[i].ptr<uint8_t>());
        }
    }
}

int FaceFinder::computeDetectionWidth(const cv::Size &inputSizeUp) const
{
    const float faceWidthMeters = 0.120; // TODO
    const float fx = m_sensor->intrinsic().m_fx;
    const float winSize = m_detector->getWindowSize().width;
    return getDetectionImageWidth(faceWidthMeters, fx, m_maxDistanceMeters, winSize, inputSizeUp.width);
}

// Side effect: set m_pyramdSizes
void FaceFinder::initACF(const cv::Size &inputSizeUp)
{
    // ### ACF (Transpose) ###
    // Find the detection image width required for object detection at the max distance:
    int detectionWidth = computeDetectionWidth(inputSizeUp);
    m_scale = float(inputSizeUp.width) / float(detectionWidth);
    
    // ACF implementation uses reduce resolution transposed image:
    cv::Size detectionSize = inputSizeUp * (1.0f/m_scale);
    cv::Mat I(detectionSize.width, detectionSize.height, CV_32FC3, cv::Scalar::all(0));
    MatP Ip(I);
    m_detector->computePyramid(Ip, m_P);
    
    m_pyramidSizes.resize(m_P.nScales);
    std::vector<ogles_gpgpu::Size2d> sizes(m_P.nScales);
    for(int i = 0; i < m_P.nScales; i++)
    {
        const auto size = m_P.data[i][0][0].size();
        sizes[i] = { size.width * 4, size.height * 4 }; // undo ACF binning x4
        m_pyramidSizes[i] = { size.width * 4, size.height * 4 };
        
        // CPU processing works with tranposed images for col-major storage assumption.
        // Undo that here to map to row major representation.  Perform this step
        // to make transpose operation explicit.
        std::swap(sizes[i].width, sizes[i].height);
        std::swap(m_pyramidSizes[i].width, m_pyramidSizes[i].height);
    }
    
    const int grayWidth = m_doLandmarks ? m_landmarksWidth : 0;
    const int flowWidth = m_doFlow ? m_flowWidth : 0;
    const ogles_gpgpu::Size2d size(inputSizeUp.width,inputSizeUp.height);
    m_acf = std::make_shared<ogles_gpgpu::ACF>(m_glContext, size, sizes, grayWidth, flowWidth, false);
    m_acf->setRotation(m_outputOrientation);
}

void FaceFinder::initFIFO(const cv::Size &inputSize)
{
    // ### Fifo ###
    m_fifo = std::make_shared<ogles_gpgpu::FifoProc>(m_scenes.size());
    m_fifo->init(inputSize.width, inputSize.height, INT_MAX, false);
    m_fifo->createFBOTex(false);
}

void FaceFinder::initFlasher()
{
    // ### Flash ###
    m_flasher = std::make_shared<ogles_gpgpu::FlashFilter>();
    m_flasher->smoothProc.setOutputSize(48, 0);
    m_acf->rgbSmoothProc.add(&m_flasher->smoothProc);
}

static ogles_gpgpu::Size2d convert(const cv::Size &size)
{
    return ogles_gpgpu::Size2d(size.width, size.height);
}

void FaceFinder::initEyeEnhancer(const cv::Size &inputSizeUp, const cv::Size &eyesSize)
{
    // ### Eye enhancer ###
    auto mode = ogles_gpgpu::EyeFilter::kLowPass;
    const float upper = 0.5;
    const float lower = 0.5;
    const float gain = 1.f;
    const float offset = 0.f;
    
    m_eyeFilter = std::make_shared<ogles_gpgpu::EyeFilter>(convert(eyesSize), mode, upper, lower, gain, offset);
    m_eyeFilter->setAutoScaling(true);
    m_eyeFilter->setOutputSize(eyesSize.width, eyesSize.height);
    
#if DRISHTI_FACEFILTER_DO_ELLIPSO_POLAR
    // Add a callback to retrieve updated eye models automatically:
    cv::Matx33f N = transformation::scale(0.5, 0.5) * transformation::translate(1.f, 1.f);
    for(int i = 0; i < 2; i++)
    {
        std::function<EyeWarp()> eyeDelegate = [&, N, i]()
        {
            auto eye = m_eyeFilter->getEyeWarps()[i];
            eye.eye = N * eye.H * eye.eye;
            return eye;
        };
        m_ellipsoPolar[i]->addEyeDelegate(eyeDelegate);
        m_eyeFilter->add(m_ellipsoPolar[i].get());
    }
#endif // DRISHTI_FACEFILTER_DO_ELLIPSO_POLAR
    
    m_eyeFilter->prepare(inputSizeUp.width, inputSizeUp.height, (GLenum)GL_RGBA);
}

void FaceFinder::initIris(const cv::Size &size)
{
    // ### Ellipsopolar warper ####
    for(int i = 0; i < 2; i++)
    {
        m_ellipsoPolar[i] = std::make_shared<ogles_gpgpu::EllipsoPolarWarp>();
        m_ellipsoPolar[i]->setOutputSize(size.width, size.height);
    }
}

void FaceFinder::initPainter(const cv::Size & /* inputSizeUp */ )
{
    
}

void FaceFinder::init(const FrameInput &frame)
{
    m_logger->info() << "FaceFinder::init()";
    m_logger->set_level(spdlog::level::err);
    
    m_start = std::chrono::system_clock::now();
    
    const cv::Size inputSize(frame.size.width, frame.size.height);

    auto inputSizeUp = inputSize;
    bool hasTranspose = ((m_outputOrientation / 90) % 2);
    if(hasTranspose)
    {
        std::swap(inputSizeUp.width, inputSizeUp.height);
    }

    m_hasInit = true;
    
    m_faceEstimator = std::make_shared<drishti::face::FaceModelEstimator>(*m_sensor);

    initColormap();
    initFIFO(inputSize);
    initACF(inputSizeUp);
    initPainter(inputSizeUp);
    initEyeEnhancer(inputSizeUp, m_eyesSize);

    if(m_doFlash)
    {
        initFlasher();
    }

    if(m_doIris)
    {
        initIris({640, 240});
    }
 }

// ogles_gpgpu::VideoSource can support list of subscribers
//
//       +=> FIFO[1][0] == ACF ====>
// VIDEO |       |
//       +=======+======== FLOW ===>

GLuint FaceFinder::operator()(const FrameInput &frame)
{
    m_logger->info() << "FaceFinder::operator() " << sBar;

    if(!m_hasInit)
    {
        m_hasInit = true;
        init2(*m_factory);
        init(frame);
    }

    assert(m_fifo->size() == m_scenes.size());

    m_frameIndex++; // increment frame index

    // Run GPU based processing on current thread and package results as a task for CPU
    // processing so that it will be available on the next frame.  This method will compute
    // ACF output using shaders on the GPU.
    ScenePrimitives scene(m_frameIndex);
    preprocess(frame, scene);

    GLuint inputTexId = m_acf->getInputTexId(), outputTexId = 0;

    inputTexId = m_acf->first()->getOutputTexId(); // override with the upright textures

    if(m_threads)
    {
        size_t inputIndex = m_fifo->getIn(); // index where inputTexture will be stored
        size_t outputIndex = m_fifo->getOut();
        outputTexId = m_fifo->getOutputFilter()->getOutputTexId();

        m_fifo->useTexture(inputTexId, 1); // uses inputIndex
        m_fifo->render();

        // Run CPU processing for this frame so that it will be available for the next frame
        m_scenes[inputIndex] = m_threads->process([scene,frame,this]()
        {
            ScenePrimitives sceneOut = scene;
            detect(frame, sceneOut);
            return sceneOut;
        });

        // Don't bother processing until FIFO is full:
        if(!m_fifo->isFull())
        {
            return m_acf->getInputTexId();
        }

        try
        {
            // Here we retrieve the CPU scene output for the previous frame,
            // which has been running on the job queue.  This adds one frame
            // of latency, but allows us to keep the GPU and CPU utilized.
            scene = m_scenes[outputIndex].get();
            
            // Get current timestamp
            const auto now = std::chrono::system_clock::now();
            double elapsedTimeSinceStart = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_start).count() / 1e3;
            
            // Perform optional frame grabbing
            // NOTE: This must occur in the main OpenGL thread:
            std::vector<cv::Mat4b> frames;
            for(auto &face : scene.faces())
            {
                for(auto &callback : m_faceMonitorCallback)
                {
                    if(callback->isValid((*face.eyesCenter), elapsedTimeSinceStart))
                    {
                        if(!frames.size())
                        {
                            dump(frames);
                        }
                        callback->grab(frames);
                    }
                }
            }
        }
        catch(...)
        {
            m_logger->error() << "Error with the output scene";
        }

        outputTexId = paint(scene, outputTexId);
    }
    else
    {
        detect(frame, scene);
        outputTexId = paint(scene, inputTexId);
    }

    return outputTexId;
}

GLuint FaceFinder::paint(const ScenePrimitives &scene, GLuint inputTexture)
{
    return inputTexture;
}

void FaceFinder::initColormap()
{
    // ##### Create a colormap ######
    cv::Mat1b colorsU8(1, 360);
    cv::Mat3b colorsU8C3;
    cv::Mat3f colors32FC3;
    for(int i = 0; i < 360; i++)
    {
        colorsU8(0,i) = uint8_t( 255.f * float(i)/(colorsU8.cols-1) + 0.5f );
    }

    cv::applyColorMap(colorsU8, colorsU8C3, cv::COLORMAP_HSV);
    colorsU8C3.convertTo(m_colors32FC3, CV_32FC3, 1.0/255.0);
}

/**
 * GPU preprocessing:
 * (1) FrameInpute -> texture -> ACF output image (P pyramid)
 * (2) Harris/Shi-Tomasi corners
 * (3) Resized grayscale image for face landmarks
 */

void FaceFinder::preprocess(const FrameInput &frame, ScenePrimitives &scene)
{
    m_logger->info() << "FaceFinder::preprocess()  " <<  int(frame.textureFormat) << sBar;
    
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);

    m_acf->setDoLuvTransfer(m_doCpuACF);
    (*m_acf)(frame);

    cv::Mat acf = m_acf->getChannels();
    assert(acf.type() == CV_8UC1);
    assert(acf.channels() == 1);
    
    auto &P = scene.m_P;
    P = std::make_shared<decltype(m_P)>();
    
    if(m_doCpuACF) // gpu
    {
        MatP LUVp = m_acf->getLuvPlanar();
        m_detector->setIsLuv(true);
        m_detector->setIsTranspose(true);
        m_detector->computePyramid(LUVp, *P);

#if DRISHTI_HCI_DEBUG_PYRAMIDS
        logPyramid("/tmp/Pgpu.png", *P);
#endif
    }
    else
    {
        if(m_acf->getChannelStatus())
        {
            fill(*P);
            
#if DRISHTI_HCI_DEBUG_PYRAMIDS
            logPyramid("/tmp/Pgpu.png", *P);
#endif
        }
    }
    
    auto flowPyramid = m_acf->getFlowPyramid();

#if DRISHTI_FACEFILTER_DO_FLOW_QUIVER || DRISHTI_FACEFILTER_DO_CORNER_PLOT
    if(m_acf->getFlowStatus())
    {
        cv::Size frameSize = uprightSize({frame.size.width, frame.size.height}, m_outputOrientation);
        cv::Mat4b ayxb = m_acf->getFlow();
        extractFlow(ayxb, frameSize, scene, 1.0f / m_acf->getFlowScale());
    }
#endif

    // ### Grayscale image ###
    if(m_doLandmarks)
    {
        scene.image() = m_acf->getGrayscale();
    }
}

void FaceFinder::fill(drishti::acf::Detector::Pyramid &P)
{
    m_acf->fill(P, m_P);
}

int FaceFinder::detect(const FrameInput &frame, ScenePrimitives &scene)
{
    m_logger->info() << "FaceFinder::detect() " << sBar;

    if(m_detector != nullptr && scene.m_P)
    {
        // Test GPU ACF detection
        // Fill in ACF Pyramid structure
        std::vector<double> scores;

        std::chrono::high_resolution_clock::time_point now = std::chrono::high_resolution_clock::now();
        double elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(now - m_objects.first).count();
        if(elapsed > m_faceFinderInterval)
        {
            (*m_detector)(*scene.m_P, scene.objects(), &scores);
            m_objects = std::make_pair(std::chrono::high_resolution_clock::now(), scene.objects());
        }
        else
        {
            scene.objects() = m_objects.second;
        }

        if(m_doLandmarks && scene.objects().size())
        {
#define LOG_DETECTIONS 0
#if LOG_DETECTIONS
            cv::Mat frame(frame.size, CV_8UC4, frame.pixelBuffer);
            for(auto &d : objects)
            {
                cv::Rect2f df(d);
                cv::rectangle(frame, {df.tl()*scale, df.br()*scale}, {0,255,0}, 1, 8);
            }
            cv::imwrite("/tmp/data/detection.png", frame);
#endif

            auto &objects = scene.objects();
            std::vector<drishti::face::FaceModel> faces(objects.size());
            for(int i = 0; i < faces.size(); i++)
            {
                faces[i].roi = objects[i];
            }

            bool isDetection = true;

            cv::Mat1b gray = scene.image();

            const float Sdr = m_scale /* acf->full */ * m_acf->getGrayscaleScale() /* full->gray */;
            cv::Matx33f Hdr(Sdr,0,0,0,Sdr,0,0,0,1); //  = cv::Matx33f::eye();
            drishti::face::FaceDetector::PaddedImage Ib(gray, {{0,0}, gray.size()});

            m_faceDetector->setDoIrisRefinement(true);
            m_faceDetector->setFaceStagesHint(8);
            m_faceDetector->setFace2StagesHint(4);
            m_faceDetector->setEyelidStagesHint(4);
            m_faceDetector->setIrisStagesHint(10);
            m_faceDetector->setIrisStagesRepetitionFactor(1);
            m_faceDetector->refine(Ib, faces, Hdr, isDetection);

            //float iod = cv::norm(faces[0].eyeFullR->irisEllipse.center - faces[0].eyeFullL->irisEllipse.center);

            // Scale faces from regression to level 0
            // The configuration sizes used in the ACF stacked channel image
            // are all upright, but the output texture used for the display
            // is still in the native (potentially rotated) coordinate system,
            // so we need to perform scaling wrt that.

            const float Srf = 1.0f / m_acf->getGrayscaleScale();
            cv::Matx33f H0(Srf,0,0,0,Srf,0,0,0,1);
            for(auto &f : faces)
            {
                f = H0 * f;
                
                // Tag each face w/ approximate distance:
                if(m_faceEstimator)
                {
                    if(f.eyeFullL.has && f.eyeFullR.has)
                    {
                        (*f.eyesCenter) = (*m_faceEstimator)(f);
                    }
                }
            }

            scene.faces() = faces;
        }
    }

    return 0;
}

// #### init2 ####

void FaceFinder::init2(drishti::face::FaceDetectorFactory &resources)
{
    m_logger->info() << "FaceFinder::init2() " << sBar;

    m_timerInfo.detectionTimeLogger = [this](double seconds)
    {
        this->m_timerInfo.detectionTime = seconds;
    };
    m_timerInfo.regressionTimeLogger = [this](double seconds)
    {
        this->m_timerInfo.regressionTime = seconds;
    };
    m_timerInfo.eyeRegressionTimeLogger = [this](double seconds)
    {
        this->m_timerInfo.eyeRegressionTime = seconds;
    };

#if DRISHTI_FACEFILTER_DO_TRACKING
    auto faceDetectorAndTracker = std::make_shared<drishti::face::FaceDetectorAndTracker>(resources);
    faceDetectorAndTracker->setMaxTrackAge(2.0);
    m_faceDetector = faceDetectorAndTracker;
#else
    m_faceDetector = std::make_shared<drishti::face::FaceDetector>(resources);
#endif
    m_faceDetector->setDoNMS(true);
    m_faceDetector->setInits(1);

    // Get weak ref to underlying ACF detector
    m_detector = dynamic_cast<drishti::acf::Detector*>(m_faceDetector->getDetector());

#if DRISHTI_FACEFILTER_DO_ACF_MODIFY
    if(m_detector)
    {
        // Perform modification
        drishti::acf::Detector::Modify dflt;
        dflt.cascThr = { "cascThr", -1.0 };
        dflt.cascCal = { "cascCal", +0.01 };
        m_detector->acfModify( dflt );
    }
#endif

    m_faceDetector->setDetectionTimeLogger(m_timerInfo.detectionTimeLogger);
    m_faceDetector->setRegressionTimeLogger(m_timerInfo.regressionTimeLogger);
    m_faceDetector->setEyeRegressionTimeLogger(m_timerInfo.eyeRegressionTimeLogger);

    {
        // FaceDetection mean:
        drishti::face::FaceModel faceDetectorMean = m_factory->getMeanFace();

        // We can change the regressor crop padding by doing a centered scaling of face features:
        if(0)
        {
            std::vector<cv::Point2f> centers
            {
                faceDetectorMean.getEyeLeftCenter(),
                faceDetectorMean.getEyeRightCenter(),
                *faceDetectorMean.noseTip
            };
            cv::Point2f center = drishti::core::centroid(centers);
            cv::Matx33f S(cv::Matx33f::diag({0.75, 0.75, 1.0}));
            cv::Matx33f T1(1,0,+center.x,0,1,+center.y,0,0,1);
            cv::Matx33f T2(1,0,-center.x,0,1,-center.y,0,0,1);
            cv::Matx33f H = T1 * S * T2;
            faceDetectorMean = H * faceDetectorMean;
        }

        m_faceDetector->setFaceDetectorMean(faceDetectorMean);
    }
}

// #### utilty: ####

static int
getDetectionImageWidth(float objectWidthMeters, float fxPixels, float zMeters, float winSizePixels, float imageWidthPixels)
{
    // objectWidthPixels / fxPixels = objectWidthMeters / zMeters
    // objectWidthPixels = (fxPixels * objectWidthMeters) / zMeters
    const float objectWidthPixels = (fxPixels * objectWidthMeters) / zMeters;
    
    // objectWidthPixels / imageWidthPixels = winSizePixels / N
    // N = winSizePixels / (objectWidthPixels / imageWidthPixels);
    return winSizePixels / (objectWidthPixels / imageWidthPixels);
}

#if DRISHTI_FACEFILTER_DO_FLOW_QUIVER || DRISHTI_FACEFILTER_DO_CORNER_PLOT
static cv::Size uprightSize(const cv::Size &size, int orientation)
{
    cv::Size upSize = size;
    if((orientation / 90) % 2)
    {
        std::swap(upSize.width, upSize.height);
    }
    return upSize;
}

static void extractCorners(const cv::Mat1b &corners, ScenePrimitives &scene, float flowScale)
{
    // ### Extract corners first: ###
    std::vector<cv::Point> points;
    try
    {
        cv::findNonZero(corners, points);
    }
    catch(...) {}
    for(const auto &p : points)
    {
        scene.corners().emplace_back(flowScale * p.x, flowScale * p.y);
    }
}

static void extractFlow(const cv::Mat4b &ayxb, const cv::Size &frameSize, ScenePrimitives &scene, float flowScale)
{
    // Compute size of flow image for just the lowest pyramid level:
    cv::Size flowSize = cv::Size2f(frameSize) * (1.0f / flowScale);
    cv::Rect flowRoi({0,0}, ayxb.size());
    flowRoi &= cv::Rect({0,0}, flowSize);

#if DRISHTI_FACEFILTER_DO_FLOW_QUIVER
    const int step = 2;
    scene.flow().reserve(scene.flow().size() + (ayxb.rows/step * ayxb.cols/step));

    for(int y = 0; y < ayxb.rows; y += step)
    {
        for(int x = 0; x < ayxb.cols; x += step)
        {
            // Extract flow:
            const cv::Vec4b &pixel = ayxb(y,x);
            cv::Point2f p(pixel[2], pixel[1]);
            cv::Point2f d = (p * (2.0f / 255.0f)) - cv::Point2f(1.0f, 1.0f);
            d *= 2.0; // additional scale for visualization
            scene.flow().emplace_back(cv::Vec4f(x, y, d.x, d.y) * flowScale);
        }
    }
#endif // DRISHTI_FACEFILTER_DO_FLOW_QUIVER

    cv::Mat1b corners;
    cv::extractChannel(ayxb(flowRoi), corners, 0);
    extractCorners(corners, scene, flowScale);
}

DRISHTI_HCI_NAMESPACE_END

#endif // DRISHTI_FACEFILTER_DO_FLOW_QUIVER || DRISHTI_FACEFILTER_DO_CORNER_PLOT
