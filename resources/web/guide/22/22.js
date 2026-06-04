function OnInit()
{
	TranslatePage();
	
	RequestProfile();
}

function HandleStudio(pVal)
{
	let strCmd=pVal['command'];
	//alert(strCmd);
	
	if(strCmd=='response_userguide_profile')
	{
		m_ProfileItem=pVal['response'];
		SortUI();
		InstallNetworkPlugin();
	}
}

function InstallNetworkPlugin()
{
	if(m_ProfileItem["network_plugin_install"]!='1' || (m_ProfileItem["network_plugin_install"]=='1' && m_ProfileItem["network_plugin_compability"]=='0') )
	{
		$("#AcceptBtn").hide();
		$("#GotoNetPluginBtn").show();
	}
}

function ReturnPreviewPage()
{
	let nMode=m_ProfileItem["model"].length;
	
	if( nMode==1)
		document.location.href="../1/index.html";
	else
		document.location.href="../21/index.html";	
}

function GotoNetPluginPage()
{
	let bRet=ResponseFilamentResult();
	
	if(bRet)
		window.location.href="../4orca/index.html";
}

function FinishGuide()
{
	let bRet=ResponseFilamentResult();
	
	if(bRet)	
	{
		var tSend={};
		tSend['sequence_id']=Math.round(new Date() / 1000);
		tSend['command']="user_guide_finish";
		tSend['data']={};
		tSend['data']['action']="finish";
		
		SendWXMessage( JSON.stringify(tSend) );	
	}
	//window.location.href="../6/index.html";
}
