#pragma once
#include "LodPolicy.h"
#include "RenderRecords.h"
#include "MosaicAverages.h"
#include "PickGrid.h"
#include <numeric>
#include <fstream>

namespace lod_tests {
using Microsoft::WRL::ComPtr;
inline void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
inline void Check(HRESULT hr, const char* message) { Require(SUCCEEDED(hr), message); }
inline double Linear(double c) { c /= 255; return c <= .04045 ? c / 12.92 : std::pow((c + .055) / 1.055, 2.4); }

inline void Policy() {
    using namespace nebula_lod;
    Histogram hist{}; hist[Bin(3)] = 47000; hist[Bin(60)] = 3000;
    Caps caps = Floors;
    UpdateCaps(caps, hist, 1.0f/60, true);
    Require(caps[0] < caps[1] && caps[1] < caps[2], "Budget centres must stay ordered");
    hist[Bin(200)] = 50000;
    UpdateCaps(caps, hist, 1.0f/60, true);
    Require(caps[1] >= caps[0]*1.3f && caps[2] >= caps[1]*1.3f, "Colliding caps collapsed a content band");
    auto low = Appearance(caps[0], caps), high = Appearance(caps[1], caps);
    Require(low.level == 4 && high.level == 0 && high.thumbnail == 0, "Mosaic endpoints must be1x1 and16x16");
    Require(Appearance(1.3f*caps[1],caps).thumbnail == 1 && Appearance(1.3f*caps[2],caps).preview == 1,
            "Image crossfade spans differ from design");
    // No orbit-relative fade: a normal-size photo well in front of the target stays opaque.
    Require(Visibility(10,20,1000,false,false,false) == 1 && Visibility(2,200,1000,false,false,false) == 1 &&
            Visibility(50,20,1000,false,false,false) == 1, "A normal-size photo in front of the target faded");
    Require(Visibility(.05f,20,1000,false,false,false) == 0 && Visibility(.2f,20,1000,false,false,false) > 0 &&
            Visibility(.2f,20,1000,false,false,false) < 1, "Near-plane guard differs");
    Require(Visibility(50,500,1000,false,false,false) == 1 && Visibility(50,950,1000,false,false,false) == 0 &&
            Visibility(.05f,950,1000,true,false,false) == 1, "Oversize safety or selection exemption differs");
    Require(Visibility(.05f,20,1000,false,true,false) == 1 &&
            Visibility(.05f,20,1000,false,false,true) == 1 &&
            Visibility(50,950,1000,false,true,true) == 0, "Near exemptions must not bypass the oversize gate");
    PickGrid picks; picks.Reserve(2,100,100); picks.Begin();
    for (int id=0; id<2; ++id) if (Visibility(id?50.0f:.05f,20,1000,false,false,false)>0) {
        Hit hit{id,0,50,50,10,10,0,static_cast<uint32_t>(id),1};
        picks.Add(hit,id);
    }
    int picked=-1; picks.Visit(50,50,[&](uint32_t id){picked=static_cast<int>(id);});
    Require(picked==1,"Fully faded photo entered the pick grid");
    Require(Contrast(75*75,false)==.42f && Contrast(15*15,false)==1,    "Distance contrast changed");
    Require(Footprint(.01f,1)==3 && Footprint(100,0)==3 && Footprint(100,1)==100,
                "Attention floor/full-attention endpoints changed");
    for (float center : {10.0f,16.0f,64.0f,256.0f}) {
        Require(DrawSize(center,center)==PointSize(center) && DrawSize(2*center,center)==2*center,
                "Bounded handoff endpoints differ");
        float previous=0;
        for (int i=1;i<=3000;++i) {
            float e=center*i/1000, drawn=DrawSize(e,center);
            Require(drawn>=previous && (e>center || drawn<=5), "Point grew beyond its cap or handoff regressed");
            Require(drawn<=6 || TileShape(drawn)==1, "Large flat disc survived the6px handoff");
            previous=drawn;
        }
        std::vector<float> ranks(50000,30);
        std::iota(ranks.begin(),ranks.begin()+3000,31.0f);
        Require(SizeCap(ranks)==30,"Size quantile admitted too many tied photos");
        std::reverse(ranks.begin(),ranks.end());
        Require(SizeCap(ranks)==30,"Size quantile depends on photo order");
        for (int frame=0;frame<200;++frame) {
            float edge=30+frame*.001f;
            std::fill(ranks.begin(),ranks.end(),edge);
            Require(SizeCap(ranks)==edge,"Size quantile stepped at a histogram-bin boundary");
        }
        std::fill(ranks.begin(),ranks.end(),0.0f);
        Require(SizeCap(ranks)==10,"Excluded photos raised the size floor");
        ranks.resize(3000); std::fill(ranks.begin(),ranks.end(),500.0f);
        Require(SizeCap(ranks)==10,"A sub-budget scene did not use the size floor");
    }
    int tier=0; uint32_t since=0;
    for (int frame=1;frame<12;++frame) {
        UpdateTier(tier,since,100,Floors,frame,false,false,false);
        Require(tier==0,"Request dwell ended early");
    }
    UpdateTier(tier,since,100,Floors,12,false,false,false);
    Require(tier==2,"Request tiers lost hysteresis/dwell");
    tier=0; since=UINT32_MAX-5;
    UpdateTier(tier,since,100,Floors,5,false,false,false);
    Require(tier==0,"Dwell failed across frame-counter wrap");
    UpdateTier(tier,since,100,Floors,6,false,false,false);
    Require(tier==2,"Dwell did not settle across frame-counter wrap");
    {
        // Wheel glide: a notch keeps its total, never overshoots, starts gently and peaks lower than
        // the former step-target camera (which took 16% of the notch in its first frame).
        float pending=-.16f, target=0, camera=0, peak=0, oldPeak=0, oldCamera=0, firstStep=0;
        float amount=1-std::exp(-(1.0f/60)/.095f);
        for (int frame=0;frame<120;++frame) {
            target+=TakeZoom(pending,1.0f/60,false);
            float step=(target-camera)*amount; camera+=step;
            float oldStep=(-.16f-oldCamera)*amount; oldCamera+=oldStep;
            if (!frame) firstStep=-step;
            Require(camera>=-.16f-1e-5f && step<=1e-7f,"Wheel glide overshot or reversed");
            peak=std::max(peak,-step); oldPeak=std::max(oldPeak,-oldStep);
        }
        Require(pending==0 && std::abs(target+.16f)<1e-5f && std::abs(camera+.16f)<1e-3f,"Wheel glide lost part of the notch");
        Require(firstStep<.16f*.05f && peak<oldPeak*.75f,"Wheel glide still kicks like a step");
        std::cout<<"Wheel glide: first frame "<<firstStep/.16f*100<<"% of a notch (was "<<oldPeak/.16f*100
                 <<"%), peak per-frame "<<peak/.16f*100<<"%\n";
    }
    std::cout<<"PASS footprint CPU policy: ordered centres, continuous content endpoints, near/size fades,\n"
             <<"     selected/lit/gather exemptions, hidden picking and unchanged contrast/dwell\n";
}

inline void WheelMotion(const std::filesystem::path& output) {
    using namespace nebula_lod;
    constexpr size_t count = 50000;
    const float kpx = 1000 * .5f / std::tan(24 * 3.14159265358979323846f / 180);
    struct Photo {
        float z, radial, focus, goal, attention, edge, raw;
        float oldDraw = 0, drawn = 0, oldDirection = 0, direction = 0;
    };
    auto distanceAttention = [](const Photo& p, float distance) {
        float radius = std::hypot(p.radial, distance + p.z);
        return std::max(1 - Band(15,40,radius),
            (.25f + .75f * (1 - Band(12,60,p.focus))) * (1 - Band(60,400,radius)));
    };
    std::array<float,2> worstOld{}, worstNew{}, oldPostStop{}, newPostStop{};
    float maxNormalizedError = 0;
    for (int direction = 0; direction < 2; ++direction)
    for (float dt : {1.0f/30, 1.0f/60, 1.0f/144})
    for (bool frozenAttention : {true, false}) {
        float distance = 25, sign = direction ? 1.0f : -1.0f;
        float target = distance * std::exp(sign * .16f);
        std::vector<Photo> photos; photos.reserve(count);
        std::vector<float> sizeEdges(count);
        Histogram initial{};
        for (size_t i = 0; i < count; ++i) {
            // Dense distant cloud plus a 6% close tail, spanning dots, both
            // handoff endpoints, all content caps and full-attention photos.
            float raw = i < 47000 ? 3 * std::pow(70.0f/3, (i+.5f)/47000) :
                70 * std::pow(256.0f/70, (i-47000+.5f)/3000);
            float z = kpx/raw - distance;
            float radial = std::min(18.0f, .25f*kpx/raw) * std::sqrt(((i*15427)%count+.5f)/count);
            Photo p{z,radial,std::hypot(radial,z)};
            p.goal = p.attention = distanceAttention(p,distance);
            p.raw = raw; p.edge = Footprint(raw,p.goal);
            sizeEdges[i] = p.edge;
            ++initial[Bin(p.edge)]; photos.push_back(p);
        }
        if (direction==0 && frozenAttention && dt==1.0f/60) {
            std::ofstream packing(output/L"footprint-packing-50k.tsv");
            packing<<"pose\tfixed10_tiles\tbudget_tiles\tsize_cap\n";
            for (float pose : {65.0f,45.0f}) {
                std::vector<float> edges; edges.reserve(count);
                for (const auto& p : photos)
                    edges.push_back(Footprint(kpx/(pose+p.z),distanceAttention(p,pose)));
                float cap=SizeCap(edges);
                size_t fixed=0,budget=0;
                for (float edge : edges) {
                    fixed+=DrawSize(edge,Floors[0])>6;
                    budget+=DrawSize(edge,cap)>6;
                }
                Require(fixed>15000 && budget<=Admissions[0],"Dense 50K cloud did not recover its tile budget");
                const char* name=pose==65?"fit":"mid";
                std::cout<<"Dense 50K packing "<<name<<": >6px fixed10="<<fixed<<" instant-budget="<<budget
                         <<" size-cap="<<cap<<'\n';
                packing<<name<<'\t'<<fixed<<'\t'<<budget<<'\t'<<cap<<'\n';
            }
            Require(packing.good(),"Write dense-cloud packing regression report");
        }
        Caps caps = Floors;
        UpdateCaps(caps,initial,dt,true);
        float sizeCap = SizeCap(sizeEdges);
        for (auto& p : photos) {
            p.oldDraw = DrawSize(p.edge,caps[0]);
            p.drawn = DrawSize(p.edge,sizeCap);
        }
        bool moving = true;
        for (int frame = 0; frame < static_cast<int>(3/dt); ++frame) {
            float seconds = frame ? dt : 1.0f/60; // Animate's first frame after idle.
            float cameraAmount = 1 - std::exp(-seconds / .095f), attentionAmount = Amount(.18f, seconds);
            // Animate's snap tolerance matters: no size may rebound after the
            // camera snaps while attention and histogram caps finish easing.
            bool wasStopped = distance == target;
            moving = std::abs(target-distance) > std::max(.0001f,target*.0005f);
            distance = moving ? distance + (target-distance)*cameraAmount : target;
            Histogram histogram{};
            for (size_t i = 0; i < photos.size(); ++i) {
                auto& p = photos[i];
                p.raw = kpx / (distance+p.z);
                if (!frozenAttention) p.goal = distanceAttention(p,distance);
                moving |= Attention(p.attention,p.goal,attentionAmount,false);
                p.edge = Footprint(p.raw,p.attention);
                ++histogram[Bin(p.edge)]; sizeEdges[i] = p.edge;
            }
            moving |= UpdateCaps(caps,histogram,seconds,false);
            float previousCap = sizeCap;
            sizeCap = SizeCap(sizeEdges);
            if (wasStopped) Require(sizeCap==previousCap,"Size cap kept moving after the camera stopped");
            for (auto& p : photos) {
                float oldDraw = DrawSize(p.edge,caps[0]), drawn = DrawSize(p.edge,sizeCap);
                float oldDelta = oldDraw-p.oldDraw, delta = drawn-p.drawn;
                if (!p.oldDirection && std::abs(oldDelta)>.00001f) p.oldDirection = std::copysign(1.0f,oldDelta);
                if (!p.direction && std::abs(delta)>.00001f) p.direction = std::copysign(1.0f,delta);
                worstOld[direction] = std::max(worstOld[direction],-p.oldDirection*oldDelta);
                worstNew[direction] = std::max(worstNew[direction],-p.direction*delta);
                Require(p.direction*delta >= -.00001f,"Wheel notch reversed a photo's size direction");
                if (wasStopped) {
                    oldPostStop[direction] = std::max(oldPostStop[direction],std::abs(oldDelta));
                    newPostStop[direction] = std::max(newPostStop[direction],std::abs(delta));
                    Require(delta==0,"Photo kept resizing after the camera stopped");
                }
                if (frozenAttention) {
                    float perspective = Footprint(p.raw,p.goal);
                    float normalized = drawn/perspective, expected = DrawSize(perspective,sizeCap)/perspective;
                    maxNormalizedError = std::max(maxNormalizedError,std::abs(normalized-expected));
                    Require(std::abs(normalized-expected) <= .000002f,
                        "Constant attention introduced an independent curve relative to the camera");
                    if (p.goal == 1 && p.raw >= 2*sizeCap) {
                        Require(std::abs(drawn/p.raw-1) <= .000002f,"Full-attention tile did not follow perspective exactly");
                    }
                }
                p.oldDraw = oldDraw; p.drawn = drawn;
            }
        }
        Require(!moving,"Wheel attention/caps failed to settle to zero-frame idle");
    }
    std::ofstream report(output/L"footprint-wheel-50k.tsv");
    report<<"direction\teased_cap_reversal_px\tinstant_cap_reversal_px\teased_cap_post_stop_px\tinstant_cap_post_stop_px\tnew_normalized_error\n";
    for (int direction = 0; direction < 2; ++direction) {
        Require(worstOld[direction] > .05f,"Wheel fixture no longer reproduces the old size bounce");
        std::cout<<"50K wheel "<<(direction?"out":"in")<<": worst reversal old="<<worstOld[direction]
                 <<"px new="<<worstNew[direction]<<"px; post-stop old="<<oldPostStop[direction]
                 <<"px new="<<newPostStop[direction]<<"px\n";
        report<<(direction?"out":"in")<<'\t'<<worstOld[direction]<<'\t'<<worstNew[direction]<<'\t'
              <<oldPostStop[direction]<<'\t'<<newPostStop[direction]<<'\t'<<maxNormalizedError<<'\n';
    }
    Require(report.good(),"Write wheel-motion regression report");
    std::cout<<"PASS wheel pipeline: 30/60/144Hz, frozen/live A/P/T, camera snap, adaptive caps, idle; "
             <<"max normalized size error="<<maxNormalizedError<<'\n';
}

struct Measurement { float width=0,height=0,alpha=0,roundness=0; std::array<double,3> color{}; };
class Gpu {
    struct Surface {
        ComPtr<ID3D11Texture2D> texture, staging;
        ComPtr<ID3D11RenderTargetView> target;
    };
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11VertexShader> vertex_,atlasVertex_;
    ComPtr<ID3D11PixelShader> pixel_;
    ComPtr<ID3D11Buffer> camera_,lens_,grid_,records_,order_,mosaic_;
    ComPtr<ID3D11ShaderResourceView> recordView_,orderView_,mosaicView_;
    std::array<ComPtr<ID3D11Texture2D>,4> images_;
    std::array<ComPtr<ID3D11ShaderResourceView>,4> imageViews_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11BlendState> coverage_;
    Surface atlasColor_,atlasMetric_,frameColor_,frameMetric_;
    float fy_ = 1/std::tan(24*3.14159265358979323846f/180);
    uint32_t mean_ = 0;

    ComPtr<ID3D11Buffer> Buffer(UINT size, UINT binding, UINT stride=0, const void* data=nullptr) {
        D3D11_BUFFER_DESC desc{}; desc.ByteWidth=size; desc.BindFlags=binding;
        if (stride) { desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; desc.StructureByteStride=stride; }
        D3D11_SUBRESOURCE_DATA initial{data,0,0}; ComPtr<ID3D11Buffer> result;
        Check(device_->CreateBuffer(&desc,data?&initial:nullptr,&result),"Create footprint test buffer"); return result;
    }
    ComPtr<ID3D11ShaderResourceView> View(ID3D11Resource* resource) {
        ComPtr<ID3D11ShaderResourceView> result;
        Check(device_->CreateShaderResourceView(resource,nullptr,&result),"Create footprint buffer view"); return result;
    }
    Surface Target(UINT width, UINT height, DXGI_FORMAT format) {
        Surface result; D3D11_TEXTURE2D_DESC desc{};
        desc.Width=width; desc.Height=height; desc.Format=format; desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
        desc.BindFlags=D3D11_BIND_RENDER_TARGET;
        Check(device_->CreateTexture2D(&desc,nullptr,&result.texture),"Create footprint readback target");
        Check(device_->CreateRenderTargetView(result.texture.Get(),nullptr,&result.target),"Create footprint RTV");
        desc.BindFlags=0; desc.Usage=D3D11_USAGE_STAGING; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        Check(device_->CreateTexture2D(&desc,nullptr,&result.staging),"Create footprint staging target");
        return result;
    }
    D3D11_MAPPED_SUBRESOURCE Read(Surface& source) {
        context_->CopyResource(source.staging.Get(),source.texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(context_->Map(source.staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Read actual footprint GPU pixels"); return mapped;
    }
    void Bind(Surface& color, Surface& metric, UINT width, UINT height, bool atlas, UINT first=0) {
        ID3D11RenderTargetView* targets[]{color.target.Get(),metric.target.Get()};
        context_->OMSetRenderTargets(2,targets,nullptr);
        const float ground[]{static_cast<float>(Linear(20)),static_cast<float>(Linear(20)),static_cast<float>(Linear(22)),1};
        const float zero[]{0,0,0,0};
        context_->ClearRenderTargetView(color.target.Get(),atlas?zero:ground);
        context_->ClearRenderTargetView(metric.target.Get(),zero);
        context_->OMSetBlendState(atlas?nullptr:coverage_.Get(),nullptr,0xffffffff);
        D3D11_VIEWPORT viewport{0,0,static_cast<float>(width),static_cast<float>(height),0,1};
        context_->RSSetViewports(1,&viewport); context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(atlas?atlasVertex_.Get():vertex_.Get(),nullptr,0);
        context_->PSSetShader(pixel_.Get(),nullptr,0);
        context_->VSSetConstantBuffers(0,1,camera_.GetAddressOf());
        context_->PSSetConstantBuffers(1,1,lens_.GetAddressOf());
        float grid[]{static_cast<float>(first),32,16,512};
        context_->UpdateSubresource(grid_.Get(),0,nullptr,grid,0,0);
        context_->VSSetConstantBuffers(2,1,grid_.GetAddressOf());
        ID3D11ShaderResourceView* geometry[]{recordView_.Get(),orderView_.Get()};
        context_->VSSetShaderResources(20,2,geometry);
        ID3D11ShaderResourceView* sources[]{imageViews_[0].Get(),imageViews_[0].Get(),imageViews_[0].Get(),imageViews_[0].Get(),
            imageViews_[1].Get(),imageViews_[2].Get(),imageViews_[3].Get(),
            mosaicView_.Get(),mosaicView_.Get(),mosaicView_.Get(),mosaicView_.Get()};
        context_->PSSetShaderResources(0,11,sources); context_->PSSetSamplers(0,1,sampler_.GetAddressOf());
    }
public:
    static constexpr UINT Width=1280, Height=1000, Count=50000;
    Gpu() {
        D3D_FEATURE_LEVEL feature;
        Check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,0,D3D11_SDK_VERSION,&device_,&feature,&context_),"Create footprint GPU test device");
        std::string program=PhotoShader;
        program+=R"(
cbuffer TestGrid:register(b2) { float4 grid; };
Varying AtlasVS(uint vertex:SV_VertexID,uint instance:SV_InstanceID) {
    Varying result=VS(vertex,instance+(uint)grid.x);
    const float2 corners[6]={float2(0,0),float2(1,0),float2(0,1),float2(0,1),float2(1,0),float2(1,1)};
    float2 cell=float2(instance%32,instance/32)*16;
    float2 pixel=cell+1+corners[vertex]*14;
    result.position=float4(pixel/grid.w*float2(2,-2)+float2(-1,1),.5,1);
    result.uv=corners[vertex];
    return result;
}
struct Measured { float4 color:SV_TARGET0; float4 metric:SV_TARGET1; };
Measured MeasurePS(Varying input) {
    PhotoOutput photo=PS(input);
    Measured result; result.color=photo.color;
    result.metric=float4(input.dimensions,photo.color.a,input.flags.z);
    return result;
}
)";
        auto code=[&](const char* entry,const char* profile) {
            ComPtr<ID3DBlob> bytes,errors;
            HRESULT hr=D3DCompile(program.data(),program.size(),entry,nullptr,nullptr,entry,profile,
                D3DCOMPILE_ENABLE_STRICTNESS,0,&bytes,&errors);
            if (FAILED(hr)&&errors) std::cerr<<static_cast<const char*>(errors->GetBufferPointer());
            Check(hr,"Compile actual footprint shader with diagnostic output"); return bytes;
        };
        auto vs=code("VS","vs_5_0"), avs=code("AtlasVS","vs_5_0"), ps=code("MeasurePS","ps_5_0");
        Check(device_->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex_),"Create footprint VS");
        Check(device_->CreateVertexShader(avs->GetBufferPointer(),avs->GetBufferSize(),nullptr,&atlasVertex_),"Create atlas VS");
        Check(device_->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&pixel_),"Create footprint PS");
        std::array<float,36> camera{}; camera[0]=camera[5]=camera[10]=camera[15]=1;
        camera[16]=fy_*Height/Width; camera[21]=fy_; camera[27]=1; camera[30]=.5f;
        camera[32]=Width; camera[33]=Height;
        camera_=Buffer(sizeof(camera),D3D11_BIND_CONSTANT_BUFFER,0,camera.data());
        float lens[8]{}; lens_=Buffer(sizeof(lens),D3D11_BIND_CONSTANT_BUFFER,0,lens);
        grid_=Buffer(16,D3D11_BIND_CONSTANT_BUFFER);
        records_=Buffer(Count*sizeof(nebula_records::Tile),D3D11_BIND_SHADER_RESOURCE,sizeof(nebula_records::Tile));
        std::vector<uint32_t> order(Count); std::iota(order.begin(),order.end(),0);
        order_=Buffer(Count*4,D3D11_BIND_SHADER_RESOURCE,4,order.data());
        recordView_=View(records_.Get()); orderView_=View(order_.Get());
        std::vector<unsigned char> original(512*512*4);
        for (UINT y=0;y<512;++y) for (UINT x=0;x<512;++x) {
            auto* p=original.data()+(y*512+x)*4;
            p[0]=static_cast<unsigned char>(90+x*90/511); p[1]=static_cast<unsigned char>(90+y*90/511);
            p[2]=((x/4+y/4)&1)?160:110; p[3]=255;
        }
        auto hierarchy=BuildMosaicAverages(original,512,0,0,512,512,1);
        mean_=0xff000000u|hierarchy[1362]|(uint32_t(hierarchy[1361])<<8)|(uint32_t(hierarchy[1360])<<16);
        for (UINT pool=0;pool<4;++pool) {
            UINT edge=64u<<pool,scale=512/edge; std::vector<unsigned char> pixels(edge*edge*4);
            for (UINT y=0;y<edge;++y) for (UINT x=0;x<edge;++x) for (UINT c=0;c<4;++c) {
                UINT sum=0;
                for (UINT dy=0;dy<scale;++dy) for (UINT dx=0;dx<scale;++dx) sum+=original[((y*scale+dy)*512+x*scale+dx)*4+c];
                pixels[(y*edge+x)*4+c]=static_cast<unsigned char>(sum/(scale*scale));
            }
            D3D11_TEXTURE2D_DESC desc{}; desc.Width=desc.Height=edge; desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
            desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA initial{pixels.data(),edge*4,0};
            Check(device_->CreateTexture2D(&desc,&initial,&images_[pool]),"Create real-sized footprint image");
            D3D11_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format=desc.Format; srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            srv.Texture2DArray.MipLevels=srv.Texture2DArray.ArraySize=1;
            Check(device_->CreateShaderResourceView(images_[pool].Get(),&srv,&imageViews_[pool]),"Create footprint image SRV");
        }
        D3D11_BUFFER_DESC raw{}; raw.ByteWidth=static_cast<UINT>(hierarchy.size()); raw.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        raw.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS; D3D11_SUBRESOURCE_DATA data{hierarchy.data(),0,0};
        Check(device_->CreateBuffer(&raw,&data,&mosaic_),"Create footprint hierarchy");
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format=DXGI_FORMAT_R32_TYPELESS; srv.ViewDimension=D3D11_SRV_DIMENSION_BUFFEREX;
        srv.BufferEx.NumElements=raw.ByteWidth/4; srv.BufferEx.Flags=D3D11_BUFFEREX_SRV_FLAG_RAW;
        Check(device_->CreateShaderResourceView(mosaic_.Get(),&srv,&mosaicView_),"Create footprint hierarchy SRV");
        D3D11_SAMPLER_DESC sampler{}; sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP; sampler.MaxLOD=D3D11_FLOAT32_MAX;
        Check(device_->CreateSamplerState(&sampler,&sampler_),"Create footprint sampler");
        D3D11_BLEND_DESC blend{}; blend.IndependentBlendEnable=TRUE;
        for (int i=0;i<2;++i) blend.RenderTarget[i].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        auto& color=blend.RenderTarget[0]; color.BlendEnable=TRUE;
        color.SrcBlend=D3D11_BLEND_SRC_ALPHA; color.DestBlend=D3D11_BLEND_INV_SRC_ALPHA; color.BlendOp=D3D11_BLEND_OP_ADD;
        color.SrcBlendAlpha=D3D11_BLEND_ONE; color.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA; color.BlendOpAlpha=D3D11_BLEND_OP_ADD;
        Check(device_->CreateBlendState(&blend,&coverage_),"Create footprint coverage blend");
        atlasColor_=Target(512,512,DXGI_FORMAT_R32G32B32A32_FLOAT);
        atlasMetric_=Target(512,512,DXGI_FORMAT_R32G32B32A32_FLOAT);
        frameColor_=Target(Width,Height,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
        frameMetric_=Target(Width,Height,DXGI_FORMAT_R32G32B32A32_FLOAT);
    }
    ~Gpu() { context_->ClearState(); context_->Flush(); }
    float Kpx() const { return Height*.5f*fy_; }
    nebula_records::Tile Record(float edge,const nebula_lod::Caps& caps,float depth=50,
                               bool selected=false,bool lit=false,bool gathered=false,bool images=true,
                               float sizeCap=nebula_lod::Floors[0]) const {
        auto c=nebula_lod::Appearance(edge,caps);
        float drawn=nebula_lod::DrawSize(edge,sizeCap);
        auto half=nebula_lod::HalfExtents(drawn,1,nebula_lod::TileShape(drawn));
        int quality=nebula_lod::Quality(3,edge,1,true);
        uint32_t token=images?static_cast<uint32_t>(quality)<<16:UINT32_MAX;
        return {{0,0,depth},half[0]*depth/Kpx(),mean_,half[1]*depth/Kpx(),1,1,
            {selected?1.0f:0,0,lit?1.0f:0,nebula_lod::Contrast(depth*depth,selected||lit)},
            {0,token,token,nebula_records::Flags(quality+2,quality+2,0,false,false,true,images?0:-1)},
            {1,edge,0,nebula_lod::Visibility(depth,drawn,Height,selected,lit,gathered)},
            {c.level,c.thumbnail,c.preview,1}};
    }
    std::vector<Measurement> Atlas(const std::vector<nebula_records::Tile>& instances) {
        Require(instances.size()<=Count,"Atlas capacity exceeded");
        D3D11_BOX box{0,0,0,static_cast<UINT>(instances.size()*sizeof(instances[0])),1,1};
        context_->UpdateSubresource(records_.Get(),0,&box,instances.data(),0,0);
        std::vector<Measurement> result(instances.size());
        for (UINT first=0;first<instances.size();first+=1024) {
            UINT count=std::min(1024u,static_cast<UINT>(instances.size())-first);
            Bind(atlasColor_,atlasMetric_,512,512,true,first); context_->DrawInstanced(6,count,0,0);
            context_->OMSetRenderTargets(0,nullptr,nullptr);
            auto colors=Read(atlasColor_), metrics=Read(atlasMetric_);
            for (UINT i=0;i<count;++i) {
                UINT x=(i%32)*16,y=(i/32)*16;
                auto& value=result[first+i];
                auto* metric=reinterpret_cast<const float*>(static_cast<unsigned char*>(metrics.pData)+(y+8)*metrics.RowPitch)+(x+8)*4;
                value.width=metric[0]; value.height=metric[1]; value.alpha=metric[2];
                value.roundness=metric[3];
                for (UINT row=4;row<12;++row) for (UINT col=4;col<12;++col) {
                    auto* pixel=reinterpret_cast<const float*>(static_cast<unsigned char*>(colors.pData)+(y+row)*colors.RowPitch)+(x+col)*4;
                    for (int c=0;c<3;++c) value.color[c]+=pixel[c]/64;
                }
            }
            context_->Unmap(atlasColor_.staging.Get(),0); context_->Unmap(atlasMetric_.staging.Get(),0);
        }
        return result;
    }
    std::pair<float,double> Physical(const nebula_records::Tile& instance) {
        D3D11_BOX box{0,0,0,sizeof(instance),1,1}; context_->UpdateSubresource(records_.Get(),0,&box,&instance,0,0);
        Bind(frameColor_,frameMetric_,Width,Height,false); context_->DrawInstanced(6,1,0,0);
        context_->OMSetRenderTargets(0,nullptr,nullptr);
        auto pixels=Read(frameColor_), metrics=Read(frameMetric_);
        auto* metric=reinterpret_cast<const float*>(static_cast<unsigned char*>(metrics.pData)+(Height/2)*metrics.RowPitch)+(Width/2)*4;
        float edge=std::max(metric[0],metric[1]); double mean=0;
        for (UINT y=Height/2-32;y<Height/2+32;++y) for (UINT x=Width/2-32;x<Width/2+32;++x) {
            auto* p=static_cast<unsigned char*>(pixels.pData)+y*pixels.RowPitch+x*4;
            mean+=(.2126*Linear(p[2])+.7152*Linear(p[1])+.0722*Linear(p[0]))/4096;
        }
        context_->Unmap(frameColor_.staging.Get(),0); context_->Unmap(frameMetric_.staging.Get(),0);
        return {edge,mean};
    }
};

inline void MotionReadback(const std::filesystem::path& output) {
    Policy(); WheelMotion(output); Gpu gpu;
    std::ofstream report(output/L"footprint-neighbours-50k.tsv");
    report<<"pose\tpairs\tmax_size_difference\tmax_color_difference\tt0\tt1\tt2\tdot_area\told_dot_area\tcoverage_ratio\tdot_budget_ratio\tmax_size_curve_error\tsize_cap\tfixed10_tiles\tbudget_tiles\n";
    for (int pose=0;pose<3;++pose) {
        float scale=pose==0?.7f:pose==1?1.0f:1.5f;
        std::vector<float> edges(Gpu::Count);
        nebula_lod::Histogram histogram{};
        for (UINT i=0;i<Gpu::Count;++i) {
            edges[i]=i<47000 ? 3+12.0f*i/47000*scale : 16*scale*std::pow(16.0f,(i-47000)/3000.0f);
            ++histogram[nebula_lod::Bin(edges[i])];
        }
        auto caps=nebula_lod::Floors; nebula_lod::UpdateCaps(caps,histogram,1.0f/60,true);
        auto ranked=edges; float sizeCap=nebula_lod::SizeCap(ranked);
        std::vector<nebula_records::Tile> instances; instances.reserve(Gpu::Count);
        for (UINT i=0;i<Gpu::Count;++i) {
            auto item=gpu.Record(edges[i],caps,50,false,false,false,true,sizeCap);
            item.position.x=((i%250+.5f)/250*2-1)*.9f*50/(2*gpu.Kpx()/Gpu::Width);
            item.position.y=((i/250+.5f)/200*2-1)*.9f*50/(2*gpu.Kpx()/Gpu::Height);
            item.effects.w=nebula_lod::Contrast(item.position.x*item.position.x+item.position.y*item.position.y+2500,false);
            instances.push_back(item);
        }
        auto read=gpu.Atlas(instances);
        size_t pairs=0,fixedTiles=0,budgetTiles=0; double maxSize=0,maxColor=0,dotArea=0,oldDotArea=0,maxSizeError=0;
        for (UINT i=0;i<Gpu::Count;++i) {
            float drawn=std::max(read[i].width,read[i].height);
            Require(std::abs(drawn-nebula_lod::DrawSize(edges[i],sizeCap))<.01f,"GPU size differs from the bounded handoff formula");
            Require(drawn<=6.0001f || read[i].roundness<=.0001f,"GPU drew a disc larger than6px");
            fixedTiles+=nebula_lod::DrawSize(edges[i],nebula_lod::Floors[0])>6;
            budgetTiles+=drawn>6;
            if (edges[i]<=sizeCap) {
                float d=nebula_lod::PointSize(edges[i]);
                Require(drawn<=5.0001f && read[i].roundness>=.9999f,"Flat-stage point became a large tile/disc");
                dotArea+=3.14159265358979323846*drawn*drawn*.25;
                oldDotArea+=3.14159265358979323846*d*d;
            }
            for (UINT delta:{1u,249u,250u,251u}) {
                UINT j=i+delta;
                if (j>=Gpu::Count || std::abs(int(i%250)-int(j%250))>1) continue;
                if (std::abs(edges[j]-edges[i])/std::min(edges[j],edges[i])>=.10f) continue;
                ++pairs;
                double size=std::abs(std::max(read[j].width,read[j].height)-std::max(read[i].width,read[i].height))/
                            std::min(std::max(read[j].width,read[j].height),std::max(read[i].width,read[i].height));
                float expectedI=nebula_lod::DrawSize(edges[i],sizeCap),expectedJ=nebula_lod::DrawSize(edges[j],sizeCap);
                double expected=std::abs(expectedJ-expectedI)/std::min(expectedI,expectedJ);
                maxSizeError=std::max(maxSizeError,std::abs(size-expected));
                // The specified cap..2*cap smoothstep intentionally amplifies
                // footprint differences here; a universal 15% limit incorrectly
                // rejects this continuous curve. Outside it retain the old gate,
                // inside it require the GPU to match the exact curve, not a wall.
                if (std::max(edges[i],edges[j])<=sizeCap || std::min(edges[i],edges[j])>=2*sizeCap)
                    Require(size<.15,"Equal-footprint neighbours diverged outside the budget handoff");
                maxSize=std::max(maxSize,size);
                for (int c=0;c<3;++c) maxColor=std::max(maxColor,std::abs(read[i].color[c]-read[j].color[c])/
                    std::max(.001,std::min(read[i].color[c],read[j].color[c])));
            }
        }
        std::cout<<"50K neighbour pose "<<pose<<": pairs="<<pairs<<" size="<<maxSize*100<<"% color="<<maxColor*100
                 <<"% size-curve error="<<maxSizeError<<'\n';
        Require(pairs>100000 && maxSizeError<.00001 && maxColor<.08,"A dot/tile cap wall remains in equal-source/equal-depth neighbours");
        Require(budgetTiles<=nebula_lod::Admissions[0],"50K background exceeded the tile budget");
        std::cout<<"50K packing pose "<<pose<<": >6px fixed10="<<fixedTiles<<" instant-budget="<<budgetTiles
                 <<" size-cap="<<sizeCap<<'\n';
        Require(oldDotArea>0 && dotArea<=oldDotArea*1.00001,        "Bounded points exceed the clamped dot area baseline");
                Require(4*dotArea<=oldDotArea*1.00001,"Sum pi*d^2 exceeds the supplied point-size budget");
                std::cout<<"50K dot coverage ratio vs clamped baseline="<<dotArea/oldDotArea<<'\n';
        report<<pose<<'\t'<<pairs<<'\t'<<maxSize<<'\t'<<maxColor<<'\t'<<caps[0]<<'\t'<<caps[1]<<'\t'<<caps[2]
              <<'\t'<<dotArea<<'\t'<<oldDotArea<<'\t'<<dotArea/oldDotArea<<'\t'<<4*dotArea/oldDotArea<<'\t'<<maxSizeError
              <<'\t'<<sizeCap<<'\t'<<fixedTiles<<'\t'<<budgetTiles<<'\n';
    }
    auto caps=nebula_lod::Caps{24,48,140};
    std::ofstream sweep(output/L"footprint-sweep.tsv");
    sweep<<"frame\te\tlevel\tthumbnail\tpreview\trendered_size\tmean_luminance\tsize_step\tluminance_step\n";
    float attention=0,lastEdge=nebula_lod::PointSize(3); double lastMean=0,maxSize=0,maxLuma=0;
    float lastLevel=4; std::array<bool,4> seen{};
    for (int frame=0;frame<720;++frame) {
        // Sweep at a constant apparent-size target, so this tests discontinuities
        // rather than the intentional acceleration inside the new growth band.
        float targetSize=nebula_lod::PointSize(3)*std::pow(500/nebula_lod::PointSize(3),frame/719.0f);
        float lo=3,hi=500;
        for (int i=0;i<24;++i) {
            float mid=(lo+hi)*.5f;
            if (nebula_lod::DrawSize(mid,nebula_lod::Floors[0])<targetSize) lo=mid; else hi=mid;
        }
        float targetEdge=(lo+hi)*.5f;
        lo=2;hi=800;
        for (int i=0;i<24;++i) {
            float raw=(lo+hi)*.5f,w=gpu.Kpx()/raw;
            float g=std::max(1-nebula_lod::Band(15,40,w),1-nebula_lod::Band(60,400,w));
            if (nebula_lod::Footprint(raw,g)<targetEdge) lo=raw; else hi=raw;
        }
        float raw=(lo+hi)*.5f,depth=gpu.Kpx()/raw;
        float proximity=1-nebula_lod::Band(15,40,depth),travel=1-nebula_lod::Band(60,400,depth);
        nebula_lod::Attention(attention,std::max(proximity,travel),nebula_lod::Amount(.18f,1.0f/60),false);
        float edge=nebula_lod::Footprint(raw,attention);
        auto c=nebula_lod::Appearance(edge,caps); auto instance=gpu.Record(edge,caps,depth);
        auto [size,mean]=gpu.Physical(instance);
        Require(std::abs(size-nebula_lod::DrawSize(edge,nebula_lod::Floors[0]))<.01f,"Sweep GPU size differs from the bounded formula");
        double sizeStep=frame?std::abs(size-lastEdge)/lastEdge:0;
        double lumaStep=frame?std::abs(mean-lastMean)/std::max(lastMean,.001):0;
        Require(size+.001f>=lastEdge && c.level<=lastLevel+.00001f,"Approach size/content regressed");
        maxSize=std::max(maxSize,sizeStep); maxLuma=std::max(maxLuma,lumaStep);
        lastEdge=size; lastMean=mean; lastLevel=c.level; seen[nebula_lod::AppearanceTier(edge,caps)]=true;
        sweep<<frame<<'\t'<<edge<<'\t'<<c.level<<'\t'<<c.thumbnail<<'\t'<<c.preview<<'\t'<<size<<'\t'<<mean<<'\t'<<sizeStep<<'\t'<<lumaStep<<'\n';
    }
    std::cout<<"Footprint sweep: max size step="<<maxSize*100<<"% fixed-box luminance step="<<maxLuma*100<<"%\n";
    Require(std::all_of(seen.begin(),seen.end(),[](bool value){return value;}) && maxSize<=.06 && maxLuma<=.06,
            "Boundary sweep exceeded a6% size/luminance jump or skipped a content stage");
    std::vector<nebula_records::Tile> fades{
        gpu.Record(20,caps,10),gpu.Record(500,caps,50),gpu.Record(950,caps,50),
        gpu.Record(950,caps,10,true),gpu.Record(950,caps,50,false,true,true)};
    auto fade=gpu.Atlas(fades);
    Require(fade[0].alpha>.999f && fade[1].alpha>.9f && fade[2].alpha==0 && fade[3].alpha>.999f && fade[4].alpha==0,
            "GPU near/oversize fade differs from CPU policy");
    auto ready=gpu.Record(80,caps), missing=gpu.Record(80,caps,50,false,false,false,false);
    auto residency=gpu.Atlas({ready,missing});
    Require(residency[0].width==residency[1].width && residency[0].height==residency[1].height,
            "Missing residency changed footprint size");
    auto fixedHandoff=gpu.Atlas({gpu.Record(15,nebula_lod::Floors),gpu.Record(15,{80,104,140})});
    Require(fixedHandoff[0].width==fixedHandoff[1].width && fixedHandoff[0].height==fixedHandoff[1].height &&
            fixedHandoff[0].roundness==fixedHandoff[1].roundness,"Adaptive content caps changed GPU size or shape");
    auto noPreview=gpu.Record(240,caps), thumbnail=noPreview;
    noPreview.pages[1]=noPreview.pages[2]=UINT32_MAX;
    thumbnail.content.z=0;
    auto below=gpu.Atlas({noPreview,thumbnail});
    for (int c=0;c<3;++c)
        Require(std::abs(below[0].color[c]-below[1].color[c])<.000001,"Missing preview did not show the thumbnail stage");
    float arrival=0; double previousMean=0,arrivalJump=0;
    for (int frame=0;frame<14;++frame) {
        auto item=gpu.Record(80,caps);
        item.content.y*=nebula_lod::Smooth(arrival);
        auto [size,mean]=gpu.Physical(item);
        Require(std::abs(size-80)<.01f,"Residency arrival changed draw size");
        if (frame) arrivalJump=std::max(arrivalJump,std::abs(mean-previousMean)/std::max(previousMean,.001));
        previousMean=mean;
        nebula_lod::Residency(arrival,true,1.0f/60,false);
    }
    Require(arrival==1 && arrivalJump<=.06,"Resident image arrival snapped instead of fading over180ms");
    Require(report.good() && sweep.good(),"Write footprint readback reports");
    std::cout<<"PASS GPU footprint: every qualifying50K grid neighbour, monotone content sweep,\n"
             <<"     <=6% size/luminance steps, no residency shrink, near fade and selected-only oversize exemption\n";
}
}
