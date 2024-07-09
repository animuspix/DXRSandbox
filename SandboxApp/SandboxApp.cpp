// DXRSandbox.cpp : Defines the entry point for the application.
//

#include "framework.h"
#include "..\CPUMemory.h"
#include "..\SandboxApp.h"
#include "Render.h"
#include "ui_constants.h"
#include "Geo.h"
#include "Scene.h"

#include <chrono>

#define MAX_LOADSTRING 100

// Global Variables:
HINSTANCE hInst;                                // current instance
WCHAR szTitle[MAX_LOADSTRING];                  // The title bar text
WCHAR szWindowClass[MAX_LOADSTRING];            // the main window class name
HWND hwnd;                                      // window handle

// Forward declarations of functions included in this code module:
ATOM                MyRegisterClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    About(HWND, UINT, WPARAM, LPARAM);

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR    lpCmdLine,
    _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // Initialize global strings
    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_DXRSANDBOX, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    // Perform application initialization:
    if (!InitInstance(hInstance, nCmdShow)) // API wrapper & core systems initialize here
    {
        return FALSE;
    }

    // Hopefully gfx API wrapper & core systems are ready by now
    ////////////////////////////////////////////////////////////

    // So I don't forget - *eventually* (once the renderer itself is fully done) I want to add imgui tools for dynamically changing film response + editing object materials, for exporting individual objects as DXRS, for
    // dynamically changing render mode, and also for moving objects around the rendered scene (that last point will require transform support in the backend + a scene wrapper/layout format)

    // In the very long term (>1 year) I would like to use this project as a sandbox for technical rendering concepts (ofc eventually including BDPT, if I ever figure it out, but probably volumetrics and layered materials first)

    // Scene/geo setup, render init
    transform modelTransform = {}; // Quite awkward - eventually we want to push these model input stuffs entirely into scene files
    modelTransform.rotation = float4(0, 0, 0, 1); // Identity quaternion
    modelTransform.translationAndScale = float4(0, 0, 0, 1); // Test model sits at the origin, unit scale

    CPUMemory::ArrayAllocHandle<Scene::Model> testModels = CPUMemory::AllocateArray<Scene::Model>(MAX_SUPPORTED_OBJ_TRANSFORMS);
    testModels[0] = { "testmodel.obj", SCENE_MODEL_FORMATS::OBJ, modelTransform }; // Implement loading for scenes/scene definitions eventually

    Scene testScene(testModels, 1);
    Geo::Init(1, &testScene);

    uint32_t numMaterials = 0;
    CPUMemory::ArrayAllocHandle<Material> sceneMaterials = {};
    Geo::SceneMaterialList(sceneMaterials, &numMaterials, 0);

    CPUMemory::SingleAllocHandle<Render::FrameConstants> frameConstants = CPUMemory::AllocateSingle<Render::FrameConstants>();

    frameConstants->screenWidth = ui::window_width;
    frameConstants->screenHeight = ui::window_height;
    frameConstants->timeSeconds = 0;
    frameConstants->fov = testScene.vfov;
    frameConstants->focalDepth = testScene.focalDepth;
    frameConstants->aberration = testScene.aberration;
    frameConstants->spp = testScene.spp;
    frameConstants->filmSPD = testScene.filmCMF;

    frameConstants->cameraTransform.translationAndScale.x = testScene.cameraPosition.x;
    frameConstants->cameraTransform.translationAndScale.y = testScene.cameraPosition.y;
    frameConstants->cameraTransform.translationAndScale.z = testScene.cameraPosition.z;
    frameConstants->cameraTransform.rotation = testScene.cameraRotation;

    for (uint32_t i = 0; i < testScene.numModels; i++)
    {
        frameConstants->sceneTransforms[i] = testScene.models[i].transformations;
    }

    frameConstants->sceneBoundsMin = testScene.sceneBoundsMin;
    frameConstants->sceneBoundsMax = testScene.sceneBoundsMax;

    frameConstants->numTransforms = testScene.numModels;

    CPUMemory::SingleAllocHandle<Render> rndr = CPUMemory::AllocateSingle<Render>();
    rndr->Init(hwnd, Render::RENDER_MODE::MODE_COMPUTE, Geo::SceneGeo(0), Geo::ViewGeo(), sceneMaterials, numMaterials, frameConstants); // Default to compute mode - simplest CPU side setup, likely easiest to test

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_DXRSANDBOX));

    // Main message loop:
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        // Upload frame constants (screen dimensions, time, deltatime, fov, focal depth, lens aberration, spp, film SPD)
        // Data comes from UI somewhere (still need to think about that), most likely modified separately and baked/updated
        // during this call
        // MAY be worthwhile having a specialist upload class - depends how those uploads are instrumented, lots of
        // complicated thoughts there
        frameConstants->timeSeconds = std::chrono::steady_clock::now().time_since_epoch().count() * 1e-9;
        rndr->UpdateFrameConstants(frameConstants);

        // Draw current frame
        rndr->Draw();
    }

    DXWrapper::Deinit();
    CPUMemory::DeInit();

    return (int)msg.wParam;
}



//
//  FUNCTION: MyRegisterClass()
//
//  PURPOSE: Registers the window class.
//
ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_DXRSANDBOX));
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName = MAKEINTRESOURCEW(IDC_DXRSANDBOX);
    wcex.lpszClassName = szWindowClass;
    wcex.hIconSm = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

    return RegisterClassExW(&wcex);
}

//
//  FUNCTION: WndProc(HWND, UINT, WPARAM, LPARAM)
//
//  PURPOSE: Processes messages for the main window.
//
//  WM_COMMAND  - process the application menu
//  WM_PAINT    - Paint the main window
//  WM_DESTROY  - post a quit message and return
//
//
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_COMMAND:
    {
        int wmId = LOWORD(wParam);
        // Parse the menu selections:
        switch (wmId)
        {
        case IDM_ABOUT:
            DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
            break;
        case IDM_EXIT:
            DestroyWindow(hWnd);
            break;
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
    }
    break;
    case WM_PAINT: // Skip processing WM_PAINT for full-rate rendering
    {
        //PAINTSTRUCT ps;
        //HDC hdc = BeginPaint(hWnd, &ps);
        //EndPaint(hWnd, &ps);
    }
    break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    case WM_NCCREATE: // Cache HWND as soon as possible (see: https://devblogs.microsoft.com/oldnewthing/20191014-00/?p=102992)
        hwnd = hWnd;
        return DefWindowProc(hWnd, message, wParam, lParam);
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

//
//   FUNCTION: InitInstance(HINSTANCE, int)
//
//   PURPOSE: Saves instance handle and creates main window
//
//   COMMENTS:
//
//        In this function, we save the instance handle in a global variable and
//        create and display the main program window.
//

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
    hInst = hInstance; // Store instance handle in our global variable
    CreateWindowW(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW,
        0, 0, ui::window_width, ui::window_height, nullptr, nullptr, hInstance, nullptr);

    if (!hwnd)
    {
        return FALSE;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Initialize core systems
    CPUMemory::Init();
    return TRUE;
}

// Message handler for about box.
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message)
    {
    case WM_INITDIALOG:
        return (INT_PTR)TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}
