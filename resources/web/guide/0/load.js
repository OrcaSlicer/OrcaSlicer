
var TargetPage=null;

function OnInit()
{
	TranslatePage();

	TargetPage=GetQueryString("target");
	
	// Orca: fallback timeout in case the C++ -> JS signal fails (e.g., WebKit issues).
	// Jump to the target page after 3 minutes so slow computers don't get stuck on a partially loaded page.
	setTimeout("JumpToTarget()",180*1000);
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