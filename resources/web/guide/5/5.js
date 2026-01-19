
function OnInit()
{
	TranslatePage();
	
	SendInstallPluginCheck();
}



function SendInstallPluginCheck()
{
	let nVal="no";
	if( $('#InstallCheck').is(':checked') ) 
		nVal="yes";
	
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="network_plugin_install";
	tSend['data']={};
	tSend['data']['action']=nVal;
	
	SendWXMessage( JSON.stringify(tSend) );		
}



function FinishGuide()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="user_guide_finish";
	tSend['data']={};
	tSend['data']['action']="finish";
	
	SendWXMessage( JSON.stringify(tSend) );	
	
	//window.location.href="../6/index.html";
}
