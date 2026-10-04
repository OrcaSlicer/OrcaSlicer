var ModelSwiper=null;
var ProfileSwiper=null;

function OnInit()
{	
	console.log(" 页面加载完成 ");
	//翻译页面
	TranslatePage();
	
	//获取数据
	RequestProjectInfo();
	
	//图片滚动器的初始化
//	mySwiper = new Swiper('.swiper', {
//		loop:true,
//		slidesPerView : 4,
//        slidesPerGroup : 1,
//		spaceBetween: 8,
//    	navigation: {
//			slidesPerGroup :4,		
//            nextEl: '.swiper-button-next',
//            prevEl: '.swiper-button-prev',
//        },
//		autoplay: {
//			delay: 3000,
//			stopOnLastSlide: false,
//			disableOnInteraction: true,
//			disableOnInteraction: false
//		}
//    });
	
    //锚点跟踪
	AddScrollEvent();
	
    //测试代码
	//ShowProjectInfo(null);
    //ShowProjectInfo(TestProjectData);
	//ShowProjectInfo(null);
    //ShowProjectInfo(TestProjectData);
//	$('#ModelPreviewList').viewer({
//		title: false,
//		fullsreen: false,
//		zIndex: 999999,
//		interval: 3000
//	});
//	$('#ModelPreviewList').viewer('update');
}

function AddScrollEvent()
{
	//跟踪页面位置
    $('#Info_Inside_Board').scroll(function(){
	//checkElementDistance("Info_Inside_Board", 'Model_Basic')
	//checkElementDistance("Info_Inside_Board", 'Model_Accessories');
	//checkElementDistance("Info_Inside_Board", 'Model_Profile');	 
	let ParentItem=$('#Info_Inside_Board');
	  
	let BItem=$('#Model_Basic');
	let AItem=$('#Model_Accessories');
	let PItem=$('#Model_Profile');
	 	
	let BTop=Math.abs(BItem.offset().top - ParentItem.offset().top);
    let BBottom=Math.abs(BItem.offset().top + BItem.innerHeight() - ParentItem.offset().top);	 
	let ATop=Math.abs(AItem.offset().top - ParentItem.offset().top);
	let ABottom=Math.abs(AItem.offset().top + AItem.innerHeight() - ParentItem.offset().top);	  
    let PTop=Math.abs(PItem.offset().top - ParentItem.offset().top);
    let PBottom=Math.abs(PItem.offset().top + PItem.innerHeight() - ParentItem.offset().top);	  
	
	console.log('------positon-----');
	console.log("B: "+BTop+'-'+BBottom);
	console.log("A: "+ATop+'-'+ABottom);
	console.log("P: "+PTop+'-'+PBottom);
	  
	let nMin=Math.min(BTop,BBottom,ATop,ABottom,PTop,PBottom );
	  
	if( nMin==BTop || nMin==BBottom)
	{
		OnMenuSelected('Model_Basic');
	}
	else if( nMin==ATop || nMin==ABottom)
	{
		OnMenuSelected('Model_Accessories');
	}
	else if( nMin==PTop || nMin==PBottom)
	{
		OnMenuSelected('Model_Profile');
	}	  
	    
    });
}

function OnMenuClick( strID )
{
	scrollLocation("Info_Inside_Board",strID);
	
	//OnMenuSelected(strID);
}

function OnMenuSelected(strID)
{
	console.log("MenuSelected:  "+strID);
	
	//UI 
	$('.LeftProcessBar').removeClass('ProcessBarSelected');
	switch(strID)
	{
		case 'Model_Basic':
			$('#Info_ProcessBar').addClass('ProcessBarSelected');
			break;
		case 'Model_Accessories':
			$('#File_ProcessBar').addClass('ProcessBarSelected');
			break;
		case 'Model_Profile':
			$('#Profile_ProcessBar').addClass('ProcessBarSelected');
			break;
	}	
}


/*-------------自动滚动跟踪效果---------*/
function scrollLocation(FatherID, ChildID) 
{
	let FItem=$('#'+FatherID);
	let CItem=$('#'+ChildID);
	
	let Tmp=CItem.offset().top - FItem.offset().top + FItem.scrollTop();
	
    // father.scrollTop(
    //     scrollTo.offset().top - father.offset().top + father.scrollTop()
    // );
    // Or you can animate the scrolling:
    FItem.animate({scrollTop:Tmp}, 400);
};

/*----------处理C++的消息-------*/
function Request3MFInfo()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="request_3mf_info";
	
	SendWXMessage( JSON.stringify(tSend) );		
}

function HandleStudio(pVal)
{
	let strCmd=pVal['command'];
	
	if(strCmd=='show_3mf_info')
	{
		ShowProjectInfo( pVal['model'] );
	}
	else if(strCmd=='clear_3mf_info')
	{
		ShowProjectInfo( null );
	}	
}

function ShowProjectInfo( p3MF )
{	
	if(p3MF==null)
	{
		$('#EmptyArea').css('display','flex');
		$('#WholeArea').hide();
		return;
	}
	
	//Check Data
	let pModel=p3MF['model'];
	let pFile=p3MF['file'];
	let pProfile=p3MF['profile'];
	
	ShowModelInfo( pModel );
    ShowFileInfo( pFile );
	ShowProfilelInfo(pProfile);

	TranslatePage();
	$('#EmptyArea').hide();
	$('#WholeArea').show();	
	
//	mySwiper = new Swiper('.swiper', {
//		loop:true,
//		spaceBetween: 8,
//    	navigation: {
//			slidesPerGroup :4,		
//            nextEl: '.swiper-button-next',
//            prevEl: '.swiper-button-prev',
//        },
//		autoplay: {
//			delay: 3000,
//			stopOnLastSlide: false,
//			disableOnInteraction: true,
//			disableOnInteraction: false
//		}
//    });
		
	AddScrollEvent();
}

function ShowModelInfo( pModel )
{
	//==========Model Info==========
	let sModelName=decodeURIComponent(pModel.name);
	let sModelAuthor=decodeURIComponent(pModel.author);
	let UploadType=pModel.upload_type.toLowerCase();
	let sLicence=pModel.license.toUpperCase();
	let sModelDesc=decodeURIComponent(pModel.description);
	
	if( pModel.hasOwnProperty('model_id') )
	{
		let m_id=pModel['model_id']+'';
		UpdateModelID( m_id.trim() );
	}
	
	SendWXDebugInfo("Model Name:  "+sModelName);
	
	$('#ModelName').text(sModelName);
	$('#ModelName').attr('title',sModelName);
    $('#ModelAuthorName').text(sModelAuthor);
	
	switch(UploadType)
	{
		case 'remix':
			$('#ModelAuthorType').attr('tid','t93');
			break;
		case 'shared':
			$('#ModelAuthorType').attr('tid','t94');
			break;			
		case 'origin':
		case 'profile':
		default:
			$('#ModelAuthorType').attr('tid','t92');
			break;			
	}
	
	switch(sLicence)
	{
		case 'CC0':
			$('#ModelLicenceImg img').attr('src','img/cc-zero.png');
			$('#ModelLicenceImg').show();
			break;
		case 'BY':
			$('#ModelLicenceImg img').attr('src','img/by.png');
			$('#ModelLicenceImg').show();			
			break;
		case 'BY-SA':
			$('#ModelLicenceImg img').attr('src','img/by-sa.png');
			$('#ModelLicenceImg').show();			
			break;
		case 'BY-ND':
			$('#ModelLicenceImg img').attr('src','img/by-nd.png');
			$('#ModelLicenceImg').show();			
			break;
		case 'BY-NC':
			$('#ModelLicenceImg img').attr('src','img/by-nc.png');
			$('#ModelLicenceImg').show();			
			break;
		case 'BY-NC-SA':
			$('#ModelLicenceImg img').attr('src','img/by-nc-sa.png');
			$('#ModelLicenceImg').show();			
			break;
		case 'BY-NC-ND':
			$('#ModelLicenceImg img').attr('src','img/by-nc-nd.png');
			$('#ModelLicenceImg').show();			
			break;	
		default:
			$('#ModelLicenceImg').hide();
			break;
	}
	
	$('#Model_Desc').empty().append( SanitizeDescHtml( html_decode(sModelDesc) ) );
			
	let ModelPreviewList=pModel.preview_img;				
    let TotalPreview=ModelPreviewList.length;
	
	if( ModelSwiper!=null )
	{
		ModelSwiper.destroy(true,true);
		ModelSwiper=null;
	}
	
    if(TotalPreview>0)
	{
		$('#ModelPreviewList').empty();
		for(let pn=0;pn<TotalPreview;pn++)			
		{	
			//let FTmpPath=decodeURIComponent(ModelPreviewList[pn]);
			let FTmpPath=ModelPreviewList[pn]['filepath'];
			
			$('#ModelPreviewList').append( $('<div class="swiper-slide"></div>').append( $('<img class="Model_PrevImg" />').attr('src',FTmpPath) ) );
		}
			
		$('#Model_Preview_Image').viewer({
			title: false,
		    fullsreen: false,
			zIndex: 11,
		    interval: 3000
	    });
		$('#Model_Preview_Image').viewer('update');
		
		//Initial Swiper
		if(TotalPreview==1)
		{
			ModelSwiper = new Swiper('#Model_Preview_Image.swiper', {
			spaceBetween: 8		
			});
			
			$('#Model_Preview_Image  .swiper-button-prev').hide();
			$('#Model_Preview_Image  .swiper-button-next').hide();
			$('#Model_Preview_Image  .swiper-pagination').hide();
		}
		else
		{
			$('#Model_Preview_Image  .swiper-button-prev').show();
			$('#Model_Preview_Image  .swiper-button-next').show();
			$('#Model_Preview_Image  .swiper-pagination').show();	
			
			ModelSwiper = new Swiper('#Model_Preview_Image.swiper', {
			loop:false,
			spaceBetween: 8,
			navigation: {
				nextEl: '.swiper-button-next',
				prevEl: '.swiper-button-prev',
			},
			autoplay: {
				delay: 3000,
				stopOnLastSlide: false,
				disableOnInteraction: false
			},
			pagination: {
				el: '.swiper-pagination',
			}				
			});
		}			
			
		$('#Model_Preview_Image').show();
	}
	else
	{
		$('#Model_Preview_Image').hide();
	}	
}
			
function ShowFileInfo( pFile )
{
	let pBOM=pFile['BOM'];
	let pAssembly=pFile['Assembly'];
	let pOther=pFile['Other'];
	
	let BTotal=pBOM.length;
	let ATotal=pAssembly.length;
	let OTotal=pOther.length;
    let fTotal=BTotal+ATotal+OTotal;
	
	//Total Number
    $('#FileTotalNum').html(fTotal);
	$('#BOMTotalNum').html(BTotal);	
	$('#AssemblyTotalNum').html(ATotal);
	$('#OtherFileTotalNum').html(OTotal);
	
	//BOM
    if(BTotal==0)
	{
		$('#FILE_BOM_List').hide();
	}
	else
	{
		ConstructFileHtml('FILE_BOM_List',pBOM);	
	}
	
	//Assembly
    if(ATotal==0)
	{
		$('#FILE_ASSEMBLY_List').hide();
	}
	else
	{
		ConstructFileHtml('FILE_ASSEMBLY_List',pAssembly);	
	}	
	
	//Other
    if(OTotal==0)
	{
		$('#FILE_OTHER_List').hide();
	}
	else
	{
		ConstructFileHtml('FILE_OTHER_List',pOther);	
	}	
	
	//zIndex
	$('.ImageIcon').viewer({
			title: false,
		    fullsreen: false,
			zIndex: 11,
		    interval: 3000
	    });
	$('.ImageIcon').viewer('update');
}


var ExcelTail=['xlsx','xlsm','xlsb','csv','xls','xltx','xltm','xlt','xlam','xla'];
var PdfTail=['pdf','fdf','xfdf','xdp','ppdf','ofd'];
var ImgTail=['jpg','jpeg','bmp','gif','svg','png','bmp'];

var ImgID=0;

function ConstructFileHtml( ID, pItem )
{
	let fTotal=pItem.length;
	
	let pBoard=$('#'+ID+'  .FileListBoard');
	pBoard.empty();
	for( let f=0;f<fTotal;f++ )
	{
		let pOne=pItem[f];
		
		let tPath=pOne['filepath'];
		let tName=decodeURIComponent(pOne['filename']);
		
		let sTail=getFileTail(tName).toLowerCase();
		
		//File or Image
		let strClass='FileIcon';
		let ImgPath='';
		
		if( $.inArray( sTail, ImgTail )>=0 )
		{
			strClass='ImageIcon';
			
			ImgPath=tPath;
		}
		else if( $.inArray( sTail, ExcelTail )>=0 )
		{			
			ImgPath='img/excel.png';			
		}
		else if( $.inArray( sTail, PdfTail )>=0 )
		{			
			ImgPath='img/pdf.png';			
		}
		else 
		{			
			ImgPath='img/default.png';			
		}		
			
		//Add html. File names come from the 3MF, so build the nodes rather than concatenating markup.
		let pIconImg=$('<img />').attr('src',ImgPath);
		let pMenu=$('<div class="FileMenu"><img src="img/s.svg" /></div>');
		if( strClass!='ImageIcon' )
		{
			pMenu.on('click', function(){ OnClickOpenFile(tPath); });
		}
		else
		{
			ImgID++;
			let TmpImgID="AF"+ImgID;
			
			pIconImg.attr('id',TmpImgID);
			pMenu.on('click', function(){ OnClickOpenImage(TmpImgID); });
		}
		
		let pFileItem=$('<div class="FileItem"></div>');
		pFileItem.append( $('<div></div>').addClass(strClass).append(pIconImg) );
		pFileItem.append( $('<div class="FileText"></div>').append( $('<div class="FileName"></div>').text(tName).attr('title',tName) ) );
		pFileItem.append( pMenu );
		pBoard.append( pFileItem );
	}
	
    if( fTotal>0 )
		$('#'+ID).show();
}


// Descriptions are untrusted 3MF metadata that may carry rich-text HTML (e.g. from MakerWorld).
// Rebuild them from an inert parse, keeping only plain formatting tags and http(s) links and images.
var DescAllowedTags=['P','BR','B','STRONG','I','EM','U','S','STRIKE','DEL','INS','SUB','SUP','SMALL','MARK',
	'Q','ABBR','KBD','WBR','H1','H2','H3','H4','H5','H6','UL','OL','LI','DL','DT','DD','BLOCKQUOTE','PRE','CODE',
	'HR','SPAN','DIV','FIGURE','FIGCAPTION','TABLE','CAPTION','THEAD','TBODY','TFOOT','TR','TH','TD','A','IMG'];
// Plain attributes kept per tag; the numeric ones must be plain non-negative integers.
var DescAllowedAttrs={'IMG':['alt','title','width','height'],'TD':['colspan','rowspan'],'TH':['colspan','rowspan'],'OL':['start']};
var DescNumericAttrs=['width','height','colspan','rowspan','start'];
// Dropped together with their content; any other unknown tag is unwrapped to its children.
var DescDroppedTags=['SCRIPT','STYLE','TEMPLATE','NOSCRIPT','TEXTAREA','TITLE','IFRAME','FRAME','OBJECT','EMBED','SVG','MATH'];

function IsHttpUrl( strUrl )
{
	// The scheme must be written as is, so nothing the URL parser would strip can precede or split it.
	if( typeof strUrl!='string' || !/^https?:/i.test(strUrl) )
		return false;
	try
	{
		let sProtocol=new URL(strUrl).protocol;
		return sProtocol=='http:' || sProtocol=='https:';
	}
	catch(e)
	{
		return false;
	}
}

// Images load as soon as the page opens, so only https sources are kept: no plain-http requests to the local network.
function IsHttpsUrl( strUrl )
{
	return IsHttpUrl(strUrl) && new URL(strUrl).protocol=='https:';
}

// Embedded YouTube players become a plain link to the video.
function GetYouTubeEmbedUrl( pNode )
{
	let sSrc=pNode.getAttribute('src');
	if( !IsHttpUrl(sSrc) )
		return null;
	let pUrl=new URL(sSrc);
	return ( pUrl.origin=='https://www.youtube.com' && pUrl.pathname.indexOf('/embed/')==0 ) ? pUrl.href : null;
}

function CopyDescNodes( pSrc, pDst )
{
	for( let pNode=pSrc.firstChild;pNode!=null;pNode=pNode.nextSibling )
	{
		if( pNode.nodeType==Node.TEXT_NODE )
		{
			pDst.appendChild( document.createTextNode(pNode.nodeValue) );
			continue;
		}
		if( pNode.nodeType!=Node.ELEMENT_NODE )
			continue;
		
		let sTag=pNode.nodeName.toUpperCase();
		if( sTag=='IFRAME' )
		{
			let sVideoUrl=GetYouTubeEmbedUrl(pNode);
			if( sVideoUrl!=null )
			{
				let pLink=document.createElement('A');
				pLink.setAttribute('href',sVideoUrl);
				pLink.textContent=sVideoUrl;
				pDst.appendChild(pLink);
			}
			continue;
		}
		if( $.inArray(sTag,DescDroppedTags)>=0 )
			continue;
		if( $.inArray(sTag,DescAllowedTags)<0 )
		{
			CopyDescNodes(pNode,pDst);
			continue;
		}
		
		let pElem=document.createElement(sTag);
		if( sTag=='A' && IsHttpUrl(pNode.getAttribute('href')) )
			pElem.setAttribute('href',pNode.getAttribute('href'));
		else if( sTag=='IMG' )
		{
			if( !IsHttpsUrl(pNode.getAttribute('src')) )
				continue;
			pElem.setAttribute('src',pNode.getAttribute('src'));
		}
		$.each( DescAllowedAttrs[sTag]||[], function(i,sAttr){
			let sValue=pNode.getAttribute(sAttr);
			if( sValue!=null && ( $.inArray(sAttr,DescNumericAttrs)<0 || /^\d+$/.test(sValue) ) )
				pElem.setAttribute(sAttr,sValue);
		});
		CopyDescNodes(pNode,pElem);
		pDst.appendChild(pElem);
	}
}

function SanitizeDescHtml( strHtml )
{
	let pFragment=document.createDocumentFragment();
	// A DOMParser document is inert: it runs no scripts and loads no resources.
	let pDoc=new DOMParser().parseFromString(strHtml,'text/html');
	if( pDoc && pDoc.body )
		CopyDescNodes(pDoc.body,pFragment);
	return pFragment;
}

function ShowProfilelInfo( pProfile )
{
	//==========Profile Info==========
	let sProfileName=decodeURIComponent(pProfile.name);
	let sProfileAuthor=decodeURIComponent(pProfile.author);
	let sProfileDesc=decodeURIComponent(pProfile.description);
	
	$('#ProfileName').text(sProfileName);
    $('#ProfileAuthor').text(sProfileAuthor);
		
	$('#Profile_Desc').empty().append( SanitizeDescHtml( html_decode(sProfileDesc) ) );
			
	let ProfilePreviewList=pProfile.preview_img;				
    let TotalPreview=ProfilePreviewList.length;
	
	if( ProfileSwiper!=null )
	{
		ProfileSwiper.destroy(true,true);
		ProfileSwiper=null;
	}	
	
    if(TotalPreview>0)
	{
		$('#ProfilePreviewList').empty();
		for(let pn=0;pn<TotalPreview;pn++)			
		{	
			let FTmpPath=ProfilePreviewList[pn]['filepath'];
			
			$('#ProfilePreviewList').append( $('<div class="swiper-slide"></div>').append( $('<img class="Model_PrevImg" />').attr('src',FTmpPath) ) );
		}
			
		$('#Profile_Preview_Image').viewer({
			title: false,
		    fullsreen: false,
		   zIndex: 11,
		    interval: 3000
	    });		
		$('#Profile_Preview_Image').viewer("update");
		
		//Init Profile Swiper
		if(TotalPreview==1)
		{
			ProfileSwiper = new Swiper('#Profile_Preview_Image.swiper', {
			spaceBetween: 8		
			});
			
			$('#Profile_Preview_Image  .swiper-button-prev').hide();
			$('#Profile_Preview_Image  .swiper-button-next').hide();
			$('#Profile_Preview_Image  .swiper-pagination').hide();			
		}
		else
		{
			$('#Profile_Preview_Image  .swiper-button-prev').show();
			$('#Profile_Preview_Image  .swiper-button-next').show();
			$('#Profile_Preview_Image  .swiper-pagination').show();					
			
			ProfileSwiper = new Swiper('#Profile_Preview_Image.swiper', {
			loop:false,
			spaceBetween: 8,
			navigation: {
				nextEl: '.swiper-button-next',
				prevEl: '.swiper-button-prev',
			},
			autoplay: {
				delay: 3000,
				stopOnLastSlide: false,
				disableOnInteraction: false
			},
			pagination: {
				el: '.swiper-pagination',
			}		
			});
		}
		
		$('#Profile_Preview_Image').show();
	}
	else
	{
		$('#Profile_Preview_Image').hide();
	}				
}			
			

//Push Command to C++		
function RequestProjectInfo()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="request_3mf_info";
		
	SendWXMessage( JSON.stringify(tSend) );		
}
			
function OnClickOpenFile( strFullPath )
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="open_3mf_accessory";
	tSend['accessory_path']=strFullPath;
	
	SendWXMessage( JSON.stringify(tSend) );
	SendWXDebugInfo('----open accessory:  '+strFullPath);
}

function OnClickOpenImage( F_ID )
{	
	$("img#"+F_ID).click();
}

function OnClickEditProjectInfo()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="edit_project_info";
		
	SendWXMessage( JSON.stringify(tSend) );		
}






