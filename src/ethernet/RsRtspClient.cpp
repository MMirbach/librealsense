// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2020 Intel Corporation. All Rights Reserved.

#include "liveMedia.hh"

#include "RsRtspClient.h"
#include <RsUsageEnvironment.h>
#include <src/ipDeviceCommon/RsCommon.h>

#include <algorithm>
#include <iostream>
#include <math.h>
#include <string>
#include <thread>
#include <vector>

#define RTSP_CLIENT_VERBOSITY_LEVEL 0 // by default, print verbose output from each "RTSPClient"
#define REQUEST_STREAMING_OVER_TCP 0

// map for stream pysical sensor
// key is generated by rs2_stream+index: depth=1,color=2,irl=3,irr=4
std::map<std::pair<int, int>, rs2_extrinsics> minimal_extrinsics_map;

std::string format_error_msg(std::string function, RsRtspReturnValue retVal)
{
    return std::string("[" + function + "] error: " + retVal.msg + " - " + std::to_string(retVal.exit_code));
}

long long int RsRTSPClient::getStreamProfileUniqueKey(rs2_video_stream t_profile)
{
    long long int key;
    key = t_profile.type * pow(10, 12) + t_profile.fmt * pow(10, 10) + t_profile.fps * pow(10, 8) + t_profile.index + t_profile.width * pow(10, 4) + t_profile.height;
    return key;
}

int RsRTSPClient::getPhysicalSensorUniqueKey(rs2_stream stream_type, int sensors_index)
{
    return stream_type * 10 + sensors_index;
}

IRsRtsp *RsRTSPClient::createNew(char const *t_rtspURL, char const *t_applicationName, portNumBits t_tunnelOverHTTPPortNum, int idx)
{
    TaskScheduler *scheduler = BasicTaskScheduler::createNew();
    UsageEnvironment *env = RSUsageEnvironment::createNew(*scheduler);

    RTSPClient::responseBufferSize = 100000;
    return (IRsRtsp *)new RsRTSPClient(scheduler, env, t_rtspURL, RTSP_CLIENT_VERBOSITY_LEVEL, t_applicationName, t_tunnelOverHTTPPortNum, idx);
}

RsRTSPClient::RsRTSPClient(TaskScheduler *t_scheduler, UsageEnvironment *t_env, char const *t_rtspURL, int t_verbosityLevel, char const *t_applicationName, portNumBits t_tunnelOverHTTPPortNum, int idx)
    : RTSPClient(*t_env, t_rtspURL, t_verbosityLevel, t_applicationName, t_tunnelOverHTTPPortNum, -1)
{
    m_lastReturnValue.exit_code = RsRtspReturnCode::OK;
    m_env = t_env;
    m_scheduler = t_scheduler;
    m_idx = idx;
}

RsRTSPClient::~RsRTSPClient() {}

std::string g_sdp[2];

std::vector<rs2_video_stream> RsRTSPClient::getStreams()
{
    if (g_sdp[m_idx].size() == 0)
    {
        this->sendDescribeCommand(this->continueAfterDESCRIBE);
    }
    else
    {
        char* buf = strdup(g_sdp[m_idx].c_str());
        this->continueAfterDESCRIBE(this, 0, buf);
    }

    // wait for continueAfterDESCRIBE to finish
    std::unique_lock<std::mutex> lck(m_commandMtx);
    m_cv.wait_for(lck, std::chrono::seconds(RTSP_CLIENT_COMMANDS_TIMEOUT_SEC), [this] { return m_commandDone; });
    // for the next command - if not done - throw timeout
    if (!m_commandDone)
    {
        RsRtspReturnValue err = {RsRtspReturnCode::ERROR_TIME_OUT, "client time out"};
        throw std::runtime_error(format_error_msg(__FUNCTION__, err));
    }
    m_commandDone = false;

    if (m_lastReturnValue.exit_code != RsRtspReturnCode::OK)
    {
        throw std::runtime_error(format_error_msg(__FUNCTION__, m_lastReturnValue));
    }

    if (this->m_supportedProfiles.size() == 0)
    {
        RsRtspReturnValue err = {RsRtspReturnCode::ERROR_GENERAL, std::string("failed to get streams from network device at url: " + std::string(this->url()))};
        throw std::runtime_error(format_error_msg(__FUNCTION__, err));
    }

    return this->m_supportedProfiles;
}

int RsRTSPClient::addStream(rs2_video_stream t_stream, rtp_callback *t_callbackObj)
{
    long long int uniqueKey = getStreamProfileUniqueKey(t_stream);
    RsMediaSubsession *subsession = this->m_subsessionMap.find(uniqueKey)->second;
    if (subsession == nullptr)
    {
        RsRtspReturnValue err = {RsRtspReturnCode::ERROR_WRONG_FLOW, "requested stream was not found"};
        throw std::runtime_error(format_error_msg(__FUNCTION__, err));
    }

    if (!subsession->initiate())
    {
        this->envir() << "Failed to initiate the subsession \n";
        RsRtspReturnValue err = {RsRtspReturnCode::ERROR_WRONG_FLOW, "Failed to initiate the subsession"};
        throw std::runtime_error(format_error_msg(__FUNCTION__, err));
    }

    // Continue setting up this subsession, by sending a RTSP "SETUP" command:
    unsigned res = this->sendSetupCommand(*subsession, this->continueAfterSETUP, False, REQUEST_STREAMING_OVER_TCP);
    // wait for continueAfterSETUP to finish
    std::unique_lock<std::mutex> lck(m_commandMtx);
    m_cv.wait_for(lck, std::chrono::seconds(RTSP_CLIENT_COMMANDS_TIMEOUT_SEC), [this] { return m_commandDone; });
    // for the next command
    if (!m_commandDone)
    {
        RsRtspReturnValue err = {RsRtspReturnCode::ERROR_TIME_OUT, "client time out"};
        throw std::runtime_error(format_error_msg(__FUNCTION__, err));
    }
    m_commandDone = false;

    if (m_lastReturnValue.exit_code != RsRtspReturnCode::OK)
    {
        throw std::runtime_error(format_error_msg(__FUNCTION__, m_lastReturnValue));
    }

    subsession->sink = RsSink::createNew(this->envir(), *subsession, t_stream, m_memPool, this->url());
    // perhaps use your own custom "MediaSink" subclass instead
    if (subsession->sink == NULL)
    {
        this->envir() << "Failed to create a data sink for the subsession: " << this->envir().getResultMsg() << "\n";
        RsRtspReturnValue err = {(RsRtspReturnCode)envir().getErrno(), std::string("Failed to create a data sink for the subsession: " + std::string(envir().getResultMsg()))};
        throw std::runtime_error(format_error_msg(__FUNCTION__, err));
    }

    subsession->miscPtr = this; // a hack to let subsession handler functions get the "RTSPClient" from the subsession
    ((RsSink *)(subsession->sink))->setCallback(t_callbackObj);
    subsession->sink->startPlaying(*(subsession->readSource()), subsessionAfterPlaying, subsession);
    // Also set a handler to be called if a RTCP "BYE" arrives for this subsession:
    if (subsession->rtcpInstance() != NULL)
    {
        subsession->rtcpInstance()->setByeWithReasonHandler(subsessionByeHandler, subsession);
    }

    return this->m_lastReturnValue.exit_code;
}

int RsRTSPClient::start()
{
    unsigned res = this->sendPlayCommand(*this->m_scs.m_session, this->continueAfterPLAY);
    // wait for continueAfterPLAY to finish
    std::unique_lock<std::mutex> lck(m_commandMtx);
    m_cv.wait_for(lck, std::chrono::seconds(RTSP_CLIENT_COMMANDS_TIMEOUT_SEC), [this] { return m_commandDone; });
    // for the next command
    if (!m_commandDone)
    {
        RsRtspReturnValue err = {RsRtspReturnCode::ERROR_TIME_OUT, "client time out"};
        throw std::runtime_error(format_error_msg(__FUNCTION__, err));
    }
    m_commandDone = false;

    if (m_lastReturnValue.exit_code != RsRtspReturnCode::OK)
    {
        throw std::runtime_error(format_error_msg(__FUNCTION__, m_lastReturnValue));
    }
    return m_lastReturnValue.exit_code;
}

int RsRTSPClient::stop()
{
    unsigned res = this->sendPauseCommand(*this->m_scs.m_session, this->continueAfterPAUSE);
    // wait for continueAfterPAUSE to finish
    std::unique_lock<std::mutex> lck(m_commandMtx);
    m_cv.wait_for(lck, std::chrono::seconds(RTSP_CLIENT_COMMANDS_TIMEOUT_SEC), [this] { return m_commandDone; });
    // for the next command
    if (!m_commandDone)
    {
        RsRtspReturnValue err = {RsRtspReturnCode::ERROR_TIME_OUT, "client time out"};
        throw std::runtime_error(format_error_msg(__FUNCTION__, err));
    }
    m_commandDone = false;
    if (m_lastReturnValue.exit_code != RsRtspReturnCode::OK)
    {
        throw std::runtime_error(format_error_msg(__FUNCTION__, m_lastReturnValue));
    }
    return m_lastReturnValue.exit_code;
}

int RsRTSPClient::close()
{
    {
        unsigned res = this->sendTeardownCommand(*this->m_scs.m_session, this->continueAfterTEARDOWN);
        // wait for continueAfterTEARDOWN to finish
        std::unique_lock<std::mutex> lck(m_commandMtx);
        m_cv.wait_for(lck, std::chrono::seconds(RTSP_CLIENT_COMMANDS_TIMEOUT_SEC), [this] { return m_commandDone; });
        // for the next command
        if (!m_commandDone)
        {
            RsRtspReturnValue err = {RsRtspReturnCode::ERROR_TIME_OUT, "client time out"};
            throw std::runtime_error(format_error_msg(__FUNCTION__, err));
        }
        m_commandDone = false;

        if (m_lastReturnValue.exit_code != RsRtspReturnCode::OK)
        {
            throw std::runtime_error(format_error_msg(__FUNCTION__, m_lastReturnValue));
        }
    }
    m_eventLoopWatchVariable = ~0;
    {
        std::lock_guard<std::mutex> lk(m_taskSchedulerMutex);
    }
    this->envir() << "Closing the stream.\n";
    UsageEnvironment *env = m_env;
    TaskScheduler *scheduler = m_scheduler;
    Medium::close(this);
    env->reclaim();
    delete scheduler;
    return m_lastReturnValue.exit_code;
}

int RsRTSPClient::setOption(const std::string &t_sensorName, rs2_option t_option, float t_value)
{
    std::string option = t_sensorName + "_" + std::to_string(t_option);
    std::string value = std::to_string(t_value);
    if (isActiveSession)
    {
        RTSPClient::sendSetParameterCommand(*this->m_scs.m_session, this->continueAfterSETCOMMAND, option.c_str(), value.c_str());
    }
    else
    {
        sendSetParameterCommand(this->continueAfterSETCOMMAND, option.c_str(), value.c_str());
    }

    std::unique_lock<std::mutex> lck(m_commandMtx);
    m_cv.wait_for(lck, std::chrono::seconds(RTSP_CLIENT_COMMANDS_TIMEOUT_SEC), [this] { return m_commandDone; });
    // for the next command
    if (!m_commandDone)
    {
        RsRtspReturnValue err = {RsRtspReturnCode::ERROR_TIME_OUT, "client time out"};
        throw std::runtime_error(format_error_msg(__FUNCTION__, err));
    }
    m_commandDone = false;

    if (m_lastReturnValue.exit_code != RsRtspReturnCode::OK)
    {
        throw std::runtime_error(format_error_msg(__FUNCTION__, m_lastReturnValue));
    }

    return m_lastReturnValue.exit_code;
}

void RsRTSPClient::setGetParamResponse(float t_res)
{
    m_getParamRes = t_res;
}

int RsRTSPClient::getOption(const std::string &t_sensorName, rs2_option t_option, float &t_value)
{
    unsigned res;
    t_value = m_getParamRes = -1;
    std::string option = t_sensorName + "_" + std::to_string(t_option);
    if (isActiveSession)
    {
        res = RTSPClient::sendGetParameterCommand(*this->m_scs.m_session, this->continueAfterGETCOMMAND, option.c_str());
    }
    else
    {
        res = sendGetParameterCommand(this->continueAfterGETCOMMAND, option.c_str());
    }
    // wait for continueAfterGETCOMMAND to finish
    std::unique_lock<std::mutex> lck(m_commandMtx);
    m_cv.wait_for(lck, std::chrono::seconds(RTSP_CLIENT_COMMANDS_TIMEOUT_SEC), [this] { return m_commandDone; });
    if (!m_commandDone)
    {
        RsRtspReturnValue err = {RsRtspReturnCode::ERROR_TIME_OUT, "client time out"};

        throw std::runtime_error(format_error_msg(__FUNCTION__, err));
    }
    m_commandDone = false;
    if (m_lastReturnValue.exit_code != RsRtspReturnCode::OK)
    {
        throw std::runtime_error(format_error_msg(__FUNCTION__, m_lastReturnValue));
    }

    t_value = m_getParamRes;

    return m_lastReturnValue.exit_code;
}

void schedulerThread(RsRTSPClient *t_rtspClientInstance)
{
    std::unique_lock<std::mutex> lk(t_rtspClientInstance->getTaskSchedulerMutex());
    t_rtspClientInstance->envir().taskScheduler().doEventLoop(&t_rtspClientInstance->getEventLoopWatchVariable());
    lk.unlock();
}

void RsRTSPClient::initFunc(MemoryPool *t_pool)
{
    std::thread thread_scheduler(schedulerThread, this);
    thread_scheduler.detach();
    m_memPool = t_pool;
}

void RsRTSPClient::setDeviceData(DeviceData t_data)
{
    m_deviceData = t_data;
}
std::vector<IpDeviceControlData> controls;

std::vector<IpDeviceControlData> RsRTSPClient::getControls()
{
    this->sendOptionsCommand(this->continueAfterOPTIONS);

    // wait for continueAfterOPTIONS to finish
    std::unique_lock<std::mutex> lck(m_commandMtx);
    m_cv.wait_for(lck, std::chrono::seconds(RTSP_CLIENT_COMMANDS_TIMEOUT_SEC), [this] { return m_commandDone; });
    // for the next command
    if (!m_commandDone)
    {
        RsRtspReturnValue err = {RsRtspReturnCode::ERROR_TIME_OUT, "client time out"};
        throw std::runtime_error(format_error_msg(__FUNCTION__, err));
    }
    m_commandDone = false;

    if (m_lastReturnValue.exit_code != RsRtspReturnCode::OK)
    {
        throw std::runtime_error(format_error_msg(__FUNCTION__, m_lastReturnValue));
    }
    return controls;
}

void updateExtrinsicsMap(rs2_video_stream videoStream, std::string extrinsics_str)
{
    std::istringstream extrinsics_stream(extrinsics_str);
    std::string s;
    while (std::getline(extrinsics_stream, s, '&'))
    {
        rs2_extrinsics extrinsics;
        int target_sensor;
        int params_count = sscanf(s.c_str(),
                                  "<to_sensor_%d>rotation:%f,%f,%f,%f,%f,%f,%f,%f,%ftranslation:%f,%f,%f",
                                  &target_sensor,
                                  &extrinsics.rotation[0],
                                  &extrinsics.rotation[1],
                                  &extrinsics.rotation[2],
                                  &extrinsics.rotation[3],
                                  &extrinsics.rotation[4],
                                  &extrinsics.rotation[5],
                                  &extrinsics.rotation[6],
                                  &extrinsics.rotation[7],
                                  &extrinsics.rotation[8],
                                  &extrinsics.translation[0],
                                  &extrinsics.translation[1],
                                  &extrinsics.translation[2]);

        // hanle NaN values
        if (params_count != SDP_EXTRINSICS_ARGS)
        {
            extrinsics = {{NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN}, {NAN, NAN, NAN}};
        }

        minimal_extrinsics_map[std::make_pair(RsRTSPClient::getPhysicalSensorUniqueKey(videoStream.type, videoStream.index), target_sensor)] = extrinsics;
    }
}

/*********************************
 *          CALLBACKS            *
 *********************************/

void RsRTSPClient::continueAfterDESCRIBE(RTSPClient *rtspClient, int resultCode, char *resultString)
{
    std::string resultStr;
    if (nullptr != resultString)
    {
        resultStr = resultString;
        delete[] resultString;
    }
    UsageEnvironment &env = rtspClient->envir();                           // alias
    RsRTSPClient *rsRtspClient = dynamic_cast<RsRTSPClient *>(rtspClient); // alias
    StreamClientState &scs = rsRtspClient->m_scs;                          // alias

    if (!resultStr.empty())
        rsRtspClient->m_lastReturnValue.msg = resultStr;
    rsRtspClient->m_lastReturnValue.exit_code = (RsRtspReturnCode)resultCode;

    do
    {
        if (resultCode != 0)
        {
            env << "Failed to get a SDP description: " << resultStr.c_str() << "\n";
            break;
        }

        g_sdp[rsRtspClient->m_idx] = resultStr;

        // Create a media session object from this SDP description(resultString):
        scs.m_session = RsMediaSession::createNew(env, resultStr.c_str());
        if (scs.m_session == NULL)
        {
            env << "Failed to create a RsMediaSession object from the SDP description: " << env.getResultMsg() << "\n";
            break;
        }
        else if (!scs.m_session->hasSubsessions())
        {
            env << "This session has no media subsessions (i.e., no \"m=\" lines)\n";
            break;
        }

        RsMediaSubsessionIterator iter(*scs.m_session);
        RsMediaSubsession *subsession = iter.next();
        while (subsession != NULL)
        {
            // Get more data from the SDP string
            const char *strWidthVal = subsession->attrVal_str("width");
            const char *strHeightVal = subsession->attrVal_str("height");
            const char *strFormatVal = subsession->attrVal_str("format");
            const char *strUidVal = subsession->attrVal_str("uid");
            const char *strFpsVal = subsession->attrVal_str("fps");
            const char *strIndexVal = subsession->attrVal_str("stream_index");
            const char *strStreamTypeVal = subsession->attrVal_str("stream_type");
            const char *strBppVal = subsession->attrVal_str("bpp");

            const char *strSerialNumVal = subsession->attrVal_str("cam_serial_num");
            const char *strCamNameVal = subsession->attrVal_str("cam_name");
            const char *strUsbTypeVal = subsession->attrVal_str("usb_type");

            int width = strWidthVal != "" ? std::stoi(strWidthVal) : 0;
            int height = strHeightVal != "" ? std::stoi(strHeightVal) : 0;
            int format = strFormatVal != "" ? std::stoi(strFormatVal) : 0;
            int uid = strUidVal != "" ? std::stoi(strUidVal) : 0;
            int fps = strFpsVal != "" ? std::stoi(strFpsVal) : 0;
            int index = strIndexVal != "" ? std::stoi(strIndexVal) : 0;
            int stream_type = strStreamTypeVal != "" ? std::stoi(strStreamTypeVal) : 0;
            int bpp = strBppVal != "" ? std::stoi(strBppVal) : 0;
            rs2_video_stream videoStream;
            videoStream.width = width;
            videoStream.height = height;
            videoStream.uid = uid;
            videoStream.fmt = static_cast<rs2_format>(format);
            videoStream.fps = fps;
            videoStream.index = index;
            videoStream.type = static_cast<rs2_stream>(stream_type);
            videoStream.bpp = bpp;

            // intrinsics desirialization should happend at once (usgin json?)
            videoStream.intrinsics.width = subsession->attrVal_int("width");
            videoStream.intrinsics.height = subsession->attrVal_int("height");
            videoStream.intrinsics.ppx = subsession->attrVal_int("ppx");
            videoStream.intrinsics.ppy = subsession->attrVal_int("ppy");
            videoStream.intrinsics.fx = subsession->attrVal_int("fx");
            videoStream.intrinsics.fy = subsession->attrVal_int("fy");
            CompressionFactory::getIsEnabled() = subsession->attrVal_bool("compression");
            videoStream.intrinsics.model = (rs2_distortion)subsession->attrVal_int("model");

            for (size_t i = 0; i < 5; i++)
            {
                videoStream.intrinsics.coeffs[i] = subsession->attrVal_int("coeff_" + i);
            }

            // extrinsics
            std::string extrinsics = subsession->attrVal_str("extrinsics");
            updateExtrinsicsMap(videoStream, extrinsics);

            DeviceData deviceData;
            deviceData.serialNum = strSerialNumVal;
            deviceData.name = strCamNameVal;
            // Return spaces back to string after getting it from the SDP
            std::replace(deviceData.name.begin(), deviceData.name.end(), '^', ' ');
            deviceData.usbType = strUsbTypeVal;
            rsRtspClient->setDeviceData(deviceData);

            long long int uniqueKey = getStreamProfileUniqueKey(videoStream);
            rsRtspClient->m_subsessionMap.insert(std::pair<long long int, RsMediaSubsession *>(uniqueKey, subsession));
            rsRtspClient->m_supportedProfiles.push_back(videoStream);
            subsession = iter.next();
            // TODO: when to delete p?
        }
    } while (0);

    {
        std::lock_guard<std::mutex> lck(rsRtspClient->m_commandMtx);
        rsRtspClient->m_commandDone = true;
    }
    rsRtspClient->m_cv.notify_one();
}

void RsRTSPClient::continueAfterSETUP(RTSPClient *rtspClient, int resultCode, char *resultString)
{
    std::string resultStr;
    if (nullptr != resultString)
    {
        resultStr = resultString;
        delete[] resultString;
    }
    UsageEnvironment &env = rtspClient->envir();                           // alias
    RsRTSPClient *rsRtspClient = dynamic_cast<RsRTSPClient *>(rtspClient); // alias
    StreamClientState &scs = rsRtspClient->m_scs;                          // alias

    env << "continueAfterSETUP " << resultCode << " " << resultStr.c_str() << "\n";

    if (!resultStr.empty())
        rsRtspClient->m_lastReturnValue.msg = resultStr;

    rsRtspClient->m_lastReturnValue.exit_code = (RsRtspReturnCode)resultCode;
    {
        std::lock_guard<std::mutex> lck(rsRtspClient->m_commandMtx);
        rsRtspClient->m_commandDone = true;
    }
    rsRtspClient->m_cv.notify_one();
}

void RsRTSPClient::continueAfterPLAY(RTSPClient *rtspClient, int resultCode, char *resultString)
{
    std::string resultStr;
    if (nullptr != resultString)
    {
        resultStr = resultString;
        delete[] resultString;
    }
    UsageEnvironment &env = rtspClient->envir();                           // alias
    RsRTSPClient *rsRtspClient = dynamic_cast<RsRTSPClient *>(rtspClient); // alias
    env << "continueAfterPLAY " << resultCode << " " << resultStr.c_str() << "\n";

    if (!resultStr.empty())
        rsRtspClient->m_lastReturnValue.msg = resultStr;
    rsRtspClient->m_lastReturnValue.exit_code = (RsRtspReturnCode)resultCode;

    {
        std::lock_guard<std::mutex> lck(rsRtspClient->m_commandMtx);
        rsRtspClient->m_commandDone = true;
    }
    rsRtspClient->m_cv.notify_one();
}

void RsRTSPClient::continueAfterTEARDOWN(RTSPClient *rtspClient, int resultCode, char *resultString)
{
    std::string resultStr;
    if (nullptr != resultString)
    {
        resultStr = resultString;
        delete[] resultString;
    }
    UsageEnvironment &env = rtspClient->envir();                           // alias
    RsRTSPClient *rsRtspClient = dynamic_cast<RsRTSPClient *>(rtspClient); // alias
    env << "continueAfterTEARDOWN " << resultCode << " " << resultStr.c_str() << "\n";

    if (!resultStr.empty())
        rsRtspClient->m_lastReturnValue.msg = resultStr;
    rsRtspClient->m_lastReturnValue.exit_code = (RsRtspReturnCode)resultCode;

    {
        std::lock_guard<std::mutex> lck(rsRtspClient->m_commandMtx);
        rsRtspClient->m_commandDone = true;
    }
    rsRtspClient->m_cv.notify_one();
}

void RsRTSPClient::continueAfterPAUSE(RTSPClient *rtspClient, int resultCode, char *resultString)
{
    std::string resultStr;
    if (nullptr != resultString)
    {
        resultStr = resultString;
        delete[] resultString;
    }
    UsageEnvironment &env = rtspClient->envir();                           // alias
    RsRTSPClient *rsRtspClient = dynamic_cast<RsRTSPClient *>(rtspClient); // alias
    env << "continueAfterPAUSE " << resultCode << " " << resultStr.c_str() << "\n";

    if (!resultStr.empty())
        rsRtspClient->m_lastReturnValue.msg = resultStr;
    rsRtspClient->m_lastReturnValue.exit_code = (RsRtspReturnCode)resultCode;

    {
        std::lock_guard<std::mutex> lck(rsRtspClient->m_commandMtx);
        rsRtspClient->m_commandDone = true;
    }
    rsRtspClient->m_cv.notify_one();
}

void RsRTSPClient::continueAfterOPTIONS(RTSPClient *rtspClient, int resultCode, char *resultString)
{
    std::string resultStr;
    if (nullptr != resultString)
    {
        resultStr = resultString;
        delete[] resultString;
    }
    UsageEnvironment &env = rtspClient->envir();                           // alias
    RsRTSPClient *rsRtspClient = dynamic_cast<RsRTSPClient *>(rtspClient); // alias
    env << "continueAfterOPTIONS " << resultCode << " " << resultStr.c_str() << "\n";

    if (!resultStr.empty())
        rsRtspClient->m_lastReturnValue.msg = resultStr;
    rsRtspClient->m_lastReturnValue.exit_code = (RsRtspReturnCode)resultCode;

    {
        std::lock_guard<std::mutex> lck(rsRtspClient->m_commandMtx);
        std::size_t foundBegin = resultStr.find_first_of("[");
        IpDeviceControlData controlData;
        int counter = 0;
        while (foundBegin != std::string::npos)
        {

            std::size_t foundEnd = resultStr.find_first_of("]", foundBegin + 1);
            std::string controlsPerSensor = resultStr.substr(foundBegin + 1, foundEnd - foundBegin);
            std::size_t pos = 0;
            while ((pos = controlsPerSensor.find(';')) != std::string::npos)
            {
                std::string controlStr = controlsPerSensor.substr(0, pos);

                controlData.sensorId = counter == 0 ? 1 : 0;
                int option_code;
                int params_count = sscanf(controlStr.c_str(), "%d{%f,%f,%f,%f}", &option_code, &controlData.range.min, &controlData.range.max, &controlData.range.def, &controlData.range.step);

                //to avoid sscanf warning
                controlData.option = (rs2_option)option_code;
                controls.push_back(controlData);
                controlsPerSensor.erase(0, pos + 1);
            }
            counter++;
            foundBegin = resultStr.find_first_of("[", foundBegin + 1);
        }
        rsRtspClient->m_commandDone = true;
    }
    rsRtspClient->m_cv.notify_one();
}

void RsRTSPClient::continueAfterSETCOMMAND(RTSPClient *rtspClient, int resultCode, char *resultString)
{
    std::string resultStr;
    if (nullptr != resultString)
    {
        resultStr = resultString;
        delete[] resultString;
    }
    UsageEnvironment &env = rtspClient->envir();                           // alias
    RsRTSPClient *rsRtspClient = dynamic_cast<RsRTSPClient *>(rtspClient); // alias
    env << "continueAfterSETCOMMAND " << resultCode << "\n";

    if (!resultStr.empty())
    {
        rsRtspClient->m_lastReturnValue.msg = resultStr;
    }
    rsRtspClient->m_lastReturnValue.exit_code = (RsRtspReturnCode)resultCode;

    {
        std::lock_guard<std::mutex> lck(rsRtspClient->m_commandMtx);
        rsRtspClient->m_commandDone = true;
    }
    rsRtspClient->m_cv.notify_one();
}

void RsRTSPClient::continueAfterGETCOMMAND(RTSPClient *rtspClient, int resultCode, char *resultString)
{
    std::string resultStr;
    if (nullptr != resultString)
    {
        resultStr = resultString;
        delete[] resultString;
    }
    UsageEnvironment &env = rtspClient->envir();                           // alias
    RsRTSPClient *rsRtspClient = dynamic_cast<RsRTSPClient *>(rtspClient); // alias
    DBG << "continueAfterGETCOMMAND: resultCode " << resultCode << ", resultString '" << resultStr.c_str();

    if (!resultStr.empty())
    {
        rsRtspClient->m_lastReturnValue.msg = resultStr;
    }
    rsRtspClient->m_lastReturnValue.exit_code = (RsRtspReturnCode)resultCode;

    if (resultCode == 0)
    {
        rsRtspClient->setGetParamResponse(std::stof(resultStr));
    }

    {
        std::lock_guard<std::mutex> lck(rsRtspClient->m_commandMtx);
        rsRtspClient->m_commandDone = true;
    }
    rsRtspClient->m_cv.notify_one();
}

// TODO: implementation
void RsRTSPClient::subsessionAfterPlaying(void *clientData)
{
    MediaSubsession *subsession = (MediaSubsession *)clientData;
    RTSPClient *rtspClient = (RTSPClient *)(subsession->miscPtr);
    rtspClient->envir() << "subsessionAfterPlaying\n";
}

void RsRTSPClient::subsessionByeHandler(void *clientData, char const *reason) {}

unsigned RsRTSPClient::sendSetParameterCommand(responseHandler *responseHandler, char const *parameterName, char const *parameterValue, Authenticator *authenticator)
{
    if (fCurrentAuthenticator < authenticator)
        fCurrentAuthenticator = *authenticator;
    char *paramString = new char[strlen(parameterName) + strlen(parameterValue) + 10];
    sprintf(paramString, "%s: %s\r\n", parameterName, parameterValue);
    unsigned result = sendRequest(new RequestRecord(++fCSeq, "SET_PARAMETER", responseHandler, NULL, NULL, False, 0.0f, -1.0f, 1.0f, paramString));
    delete[] paramString;
    return result;
}

unsigned RsRTSPClient::sendGetParameterCommand(responseHandler *responseHandler, char const *parameterName, Authenticator *authenticator)
{
    if (fCurrentAuthenticator < authenticator)
        fCurrentAuthenticator = *authenticator;

    // We assume that:
    //    parameterName is NULL means: Send no body in the request.
    //    parameterName is "" means: Send only \r\n in the request body.
    //    parameterName is non-empty means: Send "<parameterName>\r\n" as the request body.
    unsigned parameterNameLen = parameterName == NULL ? 0 : strlen(parameterName);
    char *paramString = new char[parameterNameLen + 3]; // the 3 is for \r\n + the '\0' byte
    if (parameterName == NULL)
    {
        paramString[0] = '\0';
    }
    else
    {
        sprintf(paramString, "%s\r\n", parameterName);
    }
    unsigned result = sendRequest(new RequestRecord(++fCSeq, "GET_PARAMETER", responseHandler, NULL, NULL, False, 0.0f, -1.0f, 1.0f, paramString));
    delete[] paramString;
    return result;
}

Boolean RsRTSPClient::setRequestFields(RequestRecord *request, char *&cmdURL, Boolean &cmdURLWasAllocated, char const *&protocolStr, char *&extraHeaders, Boolean &extraHeadersWereAllocated)
{
    // Set various fields that will appear in our outgoing request, depending upon the particular command that we are sending.
    if (request == nullptr)
    {
        return False;
    }
    if ((strcmp(request->commandName(), "SET_PARAMETER") == 0 || strcmp(request->commandName(), "GET_PARAMETER") == 0) && (request->session() == NULL))
    {
        cmdURL = new char[4];

        cmdURLWasAllocated = True; //use BaseUrl
        sprintf(cmdURL, "%s", "*");
    }
    else
    {
        return RTSPClient::setRequestFields(request, cmdURL, cmdURLWasAllocated, protocolStr, extraHeaders, extraHeadersWereAllocated);
    }

    return True;
}
