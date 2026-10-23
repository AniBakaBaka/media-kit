#include "comparison_labels.h"
#include "common.h"
#include <memory>
#include <type_traits>
#include <string>

namespace {
struct DeleteGdi { void operator()(void* p) const { if(p) DeleteObject(p); } };
struct DeleteDc { void operator()(HDC p) const { if(p) DeleteDC(p); } };
using Gdi=std::unique_ptr<void,DeleteGdi>;
}

Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> CreateComparisonLabels(
    ID3D11Device* device,unsigned inputWidth,unsigned inputHeight,unsigned outputWidth,unsigned outputHeight) {
  constexpr int width=768,height=240,atlasHeight=height*2+64;
  BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth=width; info.bmiHeader.biHeight=-atlasHeight;
  info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
  void* pixels=nullptr;
  Gdi bitmap(CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&pixels,nullptr,0));
  Require(bitmap && pixels,"Comparison label bitmap");
  std::unique_ptr<std::remove_pointer_t<HDC>,DeleteDc> dc(CreateCompatibleDC(nullptr));
  Require(dc!=nullptr,"Comparison label canvas");
  SelectObject(dc.get(),bitmap.get());
  SetBkMode(dc.get(),TRANSPARENT);
  auto fill=[&](RECT rect,COLORREF color) {
    Gdi brush(CreateSolidBrush(color)); FillRect(dc.get(),&rect,static_cast<HBRUSH>(brush.get()));
  };
  auto text=[&](const wchar_t* value,RECT rect,int size,COLORREF color) {
    Gdi font(CreateFontW(-size,0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Segoe UI"));
    Require(font!=nullptr,"Comparison label font");
    auto previous=SelectObject(dc.get(),font.get()); SetTextColor(dc.get(),color);
    DrawTextW(dc.get(),value,-1,&rect,DT_SINGLELINE|DT_CENTER|DT_VCENTER|DT_NOPREFIX);
    SelectObject(dc.get(),previous);
  };
  for(int row=0;row<2;++row) {
    const int y=row*height;
    const COLORREF accent=row ? RGB(118,185,0) : RGB(156,163,175);
    fill({0,y,width,y+height},RGB(16,18,20));
    fill({0,y,10,y+height},accent);
    // A small processor icon, drawn from rectangles rather than a brand logo.
    fill({48,y+46,116,y+114},accent);
    fill({56,y+54,108,y+106},RGB(16,18,20));
    fill({68,y+66,96,y+94},accent);
    for(int pin=0;pin<3;++pin) {
      const int offset=59+pin*19;
      fill({offset,y+36,offset+8,y+46},accent);
      fill({offset,y+114,offset+8,y+124},accent);
      fill({38,y+offset-12,48,y+offset-4},accent);
      fill({116,y+offset-12,126,y+offset-4},accent);
    }
    text(L"RTX",{152,y+14,511,y+58},35,RGB(209,215,221));
    text(L"DLSS 5",{148,y+50,520,y+142},75,RGB(255,255,255));
    fill({548,y+36,730,y+124},row ? accent : RGB(52,56,62));
    text(row ? L"ON" : L"OFF",{548,y+34,730,y+126},57,
        row ? RGB(12,20,0) : RGB(230,233,237));
    fill({32,y+158,736,y+160},RGB(55,60,64));
    const auto resolution=std::to_wstring(row ? outputWidth : inputWidth)+L" \u00d7 "+
        std::to_wstring(row ? outputHeight : inputHeight);
    text(resolution.c_str(),{25,y+169,424,y+231},46,RGB(235,238,242));
    text(L"FPS",{652,y+174,740,y+236},38,RGB(170,180,187));
  }
  fill({0,height*2,width,atlasHeight},RGB(16,18,20));
  const wchar_t digits[]=L"0123456789.- ";
  for(int digit=0;digit<13;++digit) {
    const wchar_t value[]={digits[digit],L'\0'};
    text(value,{digit*32,height*2,(digit+1)*32,atlasHeight},51,RGB(235,238,242));
  }
  GdiFlush();
  D3D11_TEXTURE2D_DESC desc{}; desc.Width=width; desc.Height=atlasHeight;
  desc.MipLevels=1; desc.ArraySize=1; desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count=1; desc.Usage=D3D11_USAGE_IMMUTABLE;
  desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA data{pixels,width*4,0};
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  Check(device->CreateTexture2D(&desc,&data,&texture),"Comparison label texture");
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
  Check(device->CreateShaderResourceView(texture.Get(),nullptr,&view),"Comparison label view");
  return view;
}
