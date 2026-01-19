
var TargetPage=null;

function OnInit()
{
	TranslatePage();

	TargetPage=GetQueryString("target");
	
	//setTimeout("JumpToTarget()",20*1000);
}

function HandleStudio( pVal )
{
	let strCmd=pVal['command'];
	
	if(strCmd=='userguide_profile_load_finish')
	{
		JumpToTarget();
	}
}

function JumpToTarget()
{
	window.open('../'+TargetPage+'/index.html','_self');
}