#include "stdafx.h"
#include <iostream>
#include "Camera.h"
#include "localtime_r.h"
#include "Logger.h"
#include "Helper.h"
#include "mainworker.h"
#include "../httpclient/HTTPClient.h"
#include "../smtpclient/SMTPClient.h"
#include "../webserver/Base64.h"
#include "SQLHelper.h"
#include "WebServer.h"
#include "../webserver/cWebem.h"
#include "../json/json.h"

#define CAMERA_POLL_INTERVAL 30

extern std::string szUserDataFolder;

CCameraHandler::CCameraHandler(void)
{
	m_seconds_counter=0;
}

CCameraHandler::~CCameraHandler(void)
{
}

void CCameraHandler::ReloadCameras()
{
	std::vector<std::string> _AddedCameras;
	boost::lock_guard<boost::mutex> l(m_mutex);
	m_cameradevices.clear();
	std::vector<std::vector<std::string> > result;
	std::vector<std::vector<std::string> >::const_iterator itt;

	result=m_sql.safe_query("SELECT ID, Name, Address, Port, Username, Password, ImageURL FROM Cameras WHERE (Enabled == 1) ORDER BY ID");
	if (result.size()>0)
	{
		_log.Log(LOG_STATUS,"Camera: settings (re)loaded");
		for (itt=result.begin(); itt!=result.end(); ++itt)
		{
			std::vector<std::string> sd=*itt;

			cameraDevice citem;
			std::stringstream s_str( sd[0] );
			s_str >> citem.ID;
			citem.Name		= sd[1];
			citem.Address	= sd[2];
			citem.Port		= atoi(sd[3].c_str());
			citem.Username	= base64_decode(sd[4]);
			citem.Password	= base64_decode(sd[5]);
			citem.ImageURL	= sd[6];
			m_cameradevices.push_back(citem);
			_AddedCameras.push_back(sd[0]);
		}
	}

	std::vector<std::string>::const_iterator ittCam;
	for (ittCam=_AddedCameras.begin(); ittCam!=_AddedCameras.end(); ++ittCam)
	{
		//Get Active Devices/Scenes
		ReloadCameraActiveDevices(*ittCam);
	}
}

void CCameraHandler::ReloadCameraActiveDevices(const std::string &CamID)
{
	cameraDevice *pCamera=GetCamera(CamID);
	if (pCamera==NULL)
		return;
	pCamera->mActiveDevices.clear();
	std::vector<std::vector<std::string> > result;
	std::vector<std::vector<std::string> >::const_iterator itt;
	result=m_sql.safe_query("SELECT ID, DevSceneType, DevSceneRowID FROM CamerasActiveDevices WHERE (CameraRowID=='%q') ORDER BY ID", CamID.c_str());
	if (result.size()>0)
	{
		for (itt=result.begin(); itt!=result.end(); ++itt)
		{
			std::vector<std::string> sd=*itt;
			cameraActiveDevice aDevice;
			std::stringstream s_str( sd[0] );
			s_str >> aDevice.ID;
			aDevice.DevSceneType=(unsigned char)atoi(sd[1].c_str());
			std::stringstream s_str2( sd[2] );
			s_str2 >> aDevice.DevSceneRowID;
			pCamera->mActiveDevices.push_back(aDevice);
		}
	}
}

//Return 0 if NO, otherwise Cam IDX
unsigned long long CCameraHandler::IsDevSceneInCamera(const unsigned char DevSceneType, const std::string &DevSceneID)
{
	unsigned long long ulID;
	std::stringstream s_str( DevSceneID );
	s_str >> ulID;
	return IsDevSceneInCamera(DevSceneType,ulID);
}

unsigned long long CCameraHandler::IsDevSceneInCamera(const unsigned char DevSceneType, const unsigned long long DevSceneID)
{
	boost::lock_guard<boost::mutex> l(m_mutex);
	std::vector<cameraDevice>::iterator itt;
	for (itt=m_cameradevices.begin(); itt!=m_cameradevices.end(); ++itt)
	{
		cameraDevice *pCamera=&(*itt);
		std::vector<cameraActiveDevice>::iterator itt2;
		for (itt2=pCamera->mActiveDevices.begin(); itt2!=pCamera->mActiveDevices.end(); ++itt2)
		{
			if (
				(itt2->DevSceneType==DevSceneType)&&
				(itt2->DevSceneRowID==DevSceneID)
				)
				return itt->ID;
		}
	}
	return 0;
}

std::string CCameraHandler::GetCameraURL(const std::string &CamID)
{
	cameraDevice* pCamera=GetCamera(CamID);
	if (pCamera==NULL)
		return "";
	return GetCameraURL(pCamera);
}

std::string CCameraHandler::GetCameraURL(const unsigned long long CamID)
{
	cameraDevice* pCamera=GetCamera(CamID);
	if (pCamera==NULL)
		return "";
	return GetCameraURL(pCamera);
}

std::string CCameraHandler::GetCameraURL(cameraDevice *pCamera)
{
	std::stringstream s_str;

	bool bHaveUPinURL = (pCamera->ImageURL.find("#USERNAME") != std::string::npos) || (pCamera->ImageURL.find("#PASSWORD") != std::string::npos);

	if ((!bHaveUPinURL)&&((pCamera->Username != "") || (pCamera->Password != "")))
		s_str << "http://" << pCamera->Username << ":" << pCamera->Password << "@" << pCamera->Address << ":" << pCamera->Port;
	else
		s_str << "http://" << pCamera->Address << ":" << pCamera->Port;
	return s_str.str();
}

cameraDevice* CCameraHandler::GetCamera(const std::string &CamID)
{
	unsigned long long ulID;
	std::stringstream s_str( CamID );
	s_str >> ulID;
	return GetCamera(ulID);
}

cameraDevice* CCameraHandler::GetCamera(const unsigned long long CamID)
{
	std::vector<cameraDevice>::iterator itt;
	for (itt=m_cameradevices.begin(); itt!=m_cameradevices.end(); ++itt)
	{
		if (itt->ID==CamID)
			return &(*itt);
	}
	return NULL;
}

bool CCameraHandler::TakeSnapshot(const std::string &CamID, std::vector<unsigned char> &camimage)
{
	unsigned long long ulID;
	std::stringstream s_str( CamID );
	s_str >> ulID;
	return TakeSnapshot(ulID,camimage);
}

bool CCameraHandler::TakeRaspberrySnapshot(std::vector<unsigned char> &camimage)
{
	std::string raspparams="-w 800 -h 600 -t 1";
	m_sql.GetPreferencesVar("RaspCamParams", raspparams);

	std::string OutputFileName = szUserDataFolder + "tempcam.jpg";

	std::string raspistillcmd="raspistill " + raspparams + " -o " + OutputFileName;
	std::remove(OutputFileName.c_str());
	
	//Get our image
	int ret=system(raspistillcmd.c_str());
	if (ret != 0)
	{
		_log.Log(LOG_ERROR, "Error executing raspistill command. returned: %d",ret);
		return false;
	}
	//If all went correct, we should have our file
	try
	{
		std::ifstream is(OutputFileName.c_str(), std::ios::in | std::ios::binary);
		if (is)
		{
			if (is.is_open())
			{
				char buf[512];
				while (is.read(buf, sizeof(buf)).gcount() > 0)
					camimage.insert(camimage.end(), buf, buf + (unsigned int)is.gcount());
				is.close();
				std::remove(OutputFileName.c_str());
				return true;
			}
		}
	}
	catch (...)
	{
		
	}

	return false;
}

bool CCameraHandler::TakeUVCSnapshot(std::vector<unsigned char> &camimage)
{
	std::string uvcparams="-S80 -B128 -C128 -G80 -x800 -y600 -q100";
	m_sql.GetPreferencesVar("UVCParams", uvcparams);
	
	std::string OutputFileName = szUserDataFolder + "tempcam.jpg";
	std::string nvcmd="uvccapture " + uvcparams+ " -o" + OutputFileName;
	std::remove(OutputFileName.c_str());

	try
	{
		//Get our image
		int ret=system(nvcmd.c_str());
		if (ret != 0)
		{
			_log.Log(LOG_ERROR, "Error executing uvccapture command. returned: %d", ret);
			return false;
		}
		//If all went correct, we should have our file
		std::ifstream is(OutputFileName.c_str(), std::ios::in | std::ios::binary);
		if (is)
		{
			if (is.is_open())
			{
				char buf[512];
				while (is.read(buf, sizeof(buf)).gcount() > 0)
					camimage.insert(camimage.end(), buf, buf + (unsigned int)is.gcount());
				is.close();
				std::remove(OutputFileName.c_str());
				return true;
			}
		}
	}
	catch (...)
	{
		
	}
	return false;
}

bool CCameraHandler::TakeSnapshot(const unsigned long long CamID, std::vector<unsigned char> &camimage)
{
	boost::lock_guard<boost::mutex> l(m_mutex);

	cameraDevice *pCamera=GetCamera(CamID);
	if (pCamera==NULL)
		return false;

	std::string szURL=GetCameraURL(pCamera);
	szURL+="/" + pCamera->ImageURL;
	stdreplace(szURL, "#USERNAME", pCamera->Username);
	stdreplace(szURL, "#PASSWORD", pCamera->Password);

	if (pCamera->ImageURL=="raspberry.cgi")
		return TakeRaspberrySnapshot(camimage);
	else if (pCamera->ImageURL=="uvccapture.cgi")
		return TakeUVCSnapshot(camimage);

	std::vector<std::string> ExtraHeaders;
	return HTTPClient::GETBinary(szURL,ExtraHeaders,camimage,5);
}

bool CCameraHandler::EmailCameraSnapshot(const std::string &CamIdx, const std::string &subject)
{
	int nValue;
	std::string sValue;
	if (!m_sql.GetPreferencesVar("EmailServer",nValue,sValue))
	{
		return false;//no email setup
	}
	if (sValue=="")
	{
		return false;//no email setup
	}
	std::vector<unsigned char> camimage;
	if (CamIdx=="")
		return false;

	if (!TakeSnapshot(CamIdx, camimage))
		return false;

	std::string EmailFrom;
	std::string EmailTo;
	std::string EmailServer=sValue;
	int EmailPort=25;
	std::string EmailUsername;
	std::string EmailPassword;
	int EmailAsAttachment=0;
	m_sql.GetPreferencesVar("EmailFrom",nValue,EmailFrom);
	m_sql.GetPreferencesVar("EmailTo",nValue,EmailTo);
	m_sql.GetPreferencesVar("EmailUsername",nValue,EmailUsername);
	m_sql.GetPreferencesVar("EmailPassword",nValue,EmailPassword);
	m_sql.GetPreferencesVar("EmailPort", EmailPort);
	m_sql.GetPreferencesVar("EmailAsAttachment", EmailAsAttachment);

	std::vector<char> filedata;
	filedata.insert(filedata.begin(),camimage.begin(),camimage.end());
	std::string imgstring;
	imgstring.insert(imgstring.end(),filedata.begin(),filedata.end());
	imgstring=base64_encode((const unsigned char*)imgstring.c_str(),filedata.size());

	std::string htmlMsg=
		"<html>\r\n"
		"<body>\r\n"
		"<img src=\"data:image/jpeg;base64,";
	htmlMsg+=
		imgstring +
		"\">\r\n"
		"</body>\r\n"
		"</html>\r\n";

	SMTPClient sclient;
	sclient.SetFrom(CURLEncode::URLDecode(EmailFrom.c_str()));
	sclient.SetTo(CURLEncode::URLDecode(EmailTo.c_str()));
	sclient.SetCredentials(base64_decode(EmailUsername),base64_decode(EmailPassword));
	sclient.SetServer(CURLEncode::URLDecode(EmailServer.c_str()),EmailPort);
	sclient.SetSubject(CURLEncode::URLDecode(subject));

	if (EmailAsAttachment==0)
		sclient.SetHTMLBody(htmlMsg);
	else
		sclient.AddAttachment(imgstring,"snapshot.jpg");
	bool bRet=sclient.SendEmail();
	return bRet;
}

//Webserver helpers
namespace http {
	namespace server {
		void CWebServer::RType_Cameras(WebEmSession & session, const request& req, Json::Value &root)
		{
			if (session.rights < 2)
				return;//Only admin user allowed

			std::string rused = request::findValue(&req, "used");

			root["status"] = "OK";
			root["title"] = "Cameras";

			std::vector<std::vector<std::string> > result;
			if (rused == "true") {
				result = m_sql.safe_query("SELECT ID, Name, Enabled, Address, Port, Username, Password, ImageURL FROM Cameras WHERE (Enabled=='1') ORDER BY ID ASC");
			}
			else {
				result = m_sql.safe_query("SELECT ID, Name, Enabled, Address, Port, Username, Password, ImageURL FROM Cameras ORDER BY ID ASC");
			}
			if (result.size() > 0)
			{
				std::vector<std::vector<std::string> >::const_iterator itt;
				int ii = 0;
				for (itt = result.begin(); itt != result.end(); ++itt)
				{
					std::vector<std::string> sd = *itt;

					root["result"][ii]["idx"] = sd[0];
					root["result"][ii]["Name"] = sd[1];
					root["result"][ii]["Enabled"] = (sd[2] == "1") ? "true" : "false";
					root["result"][ii]["Address"] = sd[3];
					root["result"][ii]["Port"] = atoi(sd[4].c_str());
					root["result"][ii]["Username"] = base64_decode(sd[5]);
					root["result"][ii]["Password"] = base64_decode(sd[6]);
					root["result"][ii]["ImageURL"] = sd[7];
					ii++;
				}
			}
		}
		std::string CWebServer::GetInternalCameraSnapshot(WebEmSession & session, const request& req)
		{
			m_retstr = "";
			std::vector<unsigned char> camimage;
			if (session.lastRequestPath.find("raspberry") != std::string::npos)
			{
				if (!m_mainworker.m_cameras.TakeRaspberrySnapshot(camimage))
					goto exitproc;
			}
			else
			{
				if (!m_mainworker.m_cameras.TakeUVCSnapshot(camimage))
					goto exitproc;
			}
			m_retstr.insert(m_retstr.begin(), camimage.begin(), camimage.end());
			session.outputfilename = "snapshot.jpg";
		exitproc:
			return m_retstr;
		}

		std::string CWebServer::GetCameraSnapshot(WebEmSession & session, const request& req)
		{
			m_retstr = "";
			std::vector<unsigned char> camimage;
			std::string idx = request::findValue(&req, "idx");
			if (idx == "")
				goto exitproc;

			if (!m_mainworker.m_cameras.TakeSnapshot(idx, camimage))
				goto exitproc;
			m_retstr.insert(m_retstr.begin(), camimage.begin(), camimage.end());
			session.outputfilename = "snapshot.jpg";
		exitproc:
			return m_retstr;
		}
	}
}
