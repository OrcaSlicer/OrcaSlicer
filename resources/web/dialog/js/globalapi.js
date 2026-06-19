
/*------------------ Date Function ------------------------*/
function GetFullToday( )
{
	var d=new Date();
	
	var nday=d.getDate();
	var nmonth=d.getMonth()+1;
	var nyear=d.getFullYear();
	
	var strM=nmonth+'';
	if( nmonth<10 )
		strM='0'+nmonth;

    var strD=nday+'';
    if( nday<10 )
	    strD='0'+nday;
		
	return nyear+'-'+strM+'-'+strD;
}

function GetFullDate()
{
	var d=new Date();
	
	var tDate={};
	
	tDate.nyear=d.getFullYear();
	tDate.nmonth=d.getMonth()+1;
	tDate.nday=d.getDate();
	
	tDate.nhour=d.getHours();
	tDate.nminute=d.getMinutes();
	tDate.nsecond=d.getSeconds();	
	
	tDate.nweek=d.getDay();
	tDate.ndate=d.getDate();
	
	var strM=tDate.nmonth+'';
	if( tDate.nmonth<10 )
		strM='0'+tDate.nmonth;

    var strD=tDate.nday+'';
    if( tDate.nday<10 )
	    strD='0'+tDate.nday;
	
	var strH=tDate.nhour+'';
	if( tDate.nhour<10 )
		strH='0'+tDate.nhour;

	var strMin=tDate.nminute+'';
	if( tDate.nminute<10 )
		strMin='0'+tDate.nminute;

	var strS=tDate.nsecond+'';
	if( tDate.nsecond<10 )
		strS='0'+tDate.nsecond;					
	
	tDate.strdate=tDate.nyear+'-'+strM+'-'+strD;
	tDate.strFulldate=tDate.strdate+' '+strH+':'+strMin+':'+strS;
	
	return tDate;
}


function Unixtimestamp2Date( nSecond )
{
	var d=new Date(nSecond*1000);
	
	var tDate={};
	
	tDate.nyear=d.getFullYear();
	tDate.nmonth=d.getMonth()+1;
	tDate.nday=d.getDate();
	
	tDate.nhour=d.getHours();
	tDate.nminute=d.getMinutes();
	tDate.nsecond=d.getSeconds();	
	
	tDate.nweek=d.getDay();
	tDate.ndate=d.getDate();
	
	var strM=tDate.nmonth+'';
	if( tDate.nmonth<10 )
		strM='0'+tDate.nmonth;

    var strD=tDate.nday+'';
    if( tDate.nday<10 )
	    strD='0'+tDate.nday;
				
	tDate.strdate=tDate.nyear+'-'+strM+'-'+strD;
	
	return tDate.strdate;
}


//------------Array Function-------------
Array.prototype.in_array = function (e) {
    let sArray= ',' + this.join(this.S) + ',';
	let skey=','+e+',';
	
	if(sArray.indexOf(skey)>=0)
		return true;
	else
		return false;
 }



//------------String Function------------------
/**
* Delete Left/Right Side Blank
*/
String.prototype.trim=function()
{
     return this.replace(/(^\s*)|(\s*$)/g, '');
}
/**
* Delete Left Side Blank
*/
String.prototype.ltrim=function()
{
     return this.replace(/(^\s*)/g,'');
}
/**
* Delete Right Side Blank
*/
String.prototype.rtrim=function()
{
     return this.replace(/(\s*$)/g,'');
}


//----------------Get Param-------------
function GetQueryString(name) 
{
	var reg = new RegExp("(^|&)"+ name +"=([^&]*)(&|$)"); 
    var r = window.location.search.substr(1).match(reg); 
    if (r!=null)
	{
		return unescape(r[2]);
	}
    else
	{
		return null; 
    }
} 

function GetGetStr()
{
	let strGet="";
	
	//获取当前URL
    let url = document.location.href;

    //获取?的位置
    let index = url.indexOf("?")
    if(index != -1) {
        //截取出?后面的字符串
        strGet = url.substr(index + 1);	
	}
	
	return strGet;
}


/*--------------------JSON  Function------------*/

/*
功能：检查一个字符串是不是标准的JSON格式
参数： strJson          被检查的字符串
返回值： 如果字符串是一个标准的JSON格式，则返回JSON对象
        如果字符串不是标准JSON格式，则返回null
*/
function IsJson( strJson )
{
	var tJson=null;
	try
	{
		tJson=JSON.parse(strJson);
	}
	catch(exception)
	{
	    return null;
	}	
	
	return tJson;
}

/*-----------------------Ajax Function--------------------*/
/*对JQuery的Ajax函数的封装，只支持异步
参数说明：
    url      目标地址
	action   post/get
	data     字符串格式的发送内容
	asyn     true---异步模式;false-----同步模式;
*/
function HttpReq( url,action, data,callbackfunc)
{
	var strAction=action.toLowerCase();
	
	if( strAction=="post")
	{
		$.post(url,data,callbackfunc);			
	}
	else if( strAction=="get")
    {
		$.get(url,callbackfunc);
	}
}

/*---------------Cookie Function-------------------*/ 
function setCookie(name, value, time='',path='') {
    if(time && path){
        var strsec = time * 1000;
        var exp = new Date();
        exp.setTime(exp.getTime() + strsec * 1);
        document.cookie = name + "=" + escape(value) + ";expires=" + exp.toGMTString() + ";path="+path;
    }else if(time){
        var strsec = time * 1000;
        var exp = new Date();
        exp.setTime(exp.getTime() + strsec * 1);
        document.cookie = name + "=" + escape(value) + ";expires=" + exp.toGMTString();
    }else if(path){
        document.cookie = name + "=" + escape(value) + ";path="+path;
    }else{
        document.cookie = name + "=" + escape(value);
    }
}

function getCookie(c_name) 
{
	if(document.cookie.length > 0) {
		c_start = document.cookie.indexOf(c_name + "=");//获取字符串的起点
	    if(c_start != -1) {
			c_start = c_start + c_name.length + 1;//获取值的起点
			c_end = document.cookie.indexOf(";", c_start);//获取结尾处
			if(c_end == -1) c_end = document.cookie.length;//如果是最后一个，结尾就是cookie字符串的结尾
			return decodeURI(document.cookie.substring(c_start, c_end));//截取字符串返回
	    }
	}
	
	return "";
}

function checkCookie(c_name) {     
    username = getCookie(c_name);     
    console.log(username);     
    if (username != null && username != "")     
    { return true; }     
    else     
    { return false;  }
}

function clearCookie(name) {     
    setCookie(name, "", -1); 
}


/*--------Studio WX Message-------*/
function IsInSlicer()
{
	let bMatch=navigator.userAgent.match(  RegExp('BBL-Slicer','i') );
	
	return bMatch;
}



function SendWXMessage( strMsg )
{
	let bCheck=IsInSlicer();
	
	if(bCheck!=null)
	{
		window.wx.postMessage(strMsg);
	}
}

/*------CSS Link Control----*/
function RemoveCssLink( LinkPath )
{
	let pNow=$("head link[href='"+LinkPath+"']");
	
	let nTotal=pNow.length;
    for( let n=0;n<nTotal;n++ )
	{
		pNow[n].remove();
	}	
}

function AddCssLink( LinkPath )
{	
	var head = document.getElementsByTagName('head')[0];
	var link = document.createElement('link');
	link.href = LinkPath;
	link.rel = 'stylesheet';
	link.type = 'text/css';
	head.appendChild(link);
}

function CheckCssLinkExist( LinkPath )
{
	let pNow=$("head link[href='"+LinkPath+"']");
	let nTotal=pNow.length;
	
	return nTotal;
}


/*------Dark Mode------*/

function SwitchDarkMode( DarkCssPath )
{		
	ExecuteDarkMode( DarkCssPath );
    setInterval("ExecuteDarkMode('"+DarkCssPath+"')",1000);	
}

function ExecuteDarkMode( DarkCssPath )
{
    let nMode=0;
	let bDarkMode=navigator.userAgent.match(  RegExp('dark','i') );	
	if( bDarkMode!=null )
		nMode=1;
	
	let nNow=CheckCssLinkExist(DarkCssPath);
	if( nMode==0 )
	{
		if(nNow>0)
			RemoveCssLink(DarkCssPath);
	}
	else
	{
		if(nNow==0)
			AddCssLink(DarkCssPath);
	}	
}

SwitchDarkMode( "../css/dark.css" );