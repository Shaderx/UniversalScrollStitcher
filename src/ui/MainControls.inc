    // Native controls and guided presentation live here; capture and export stay
    // in MainWindow so a layout change cannot alter the stitching pipeline.
    enum class UiStage { Choose, Calibrate, Capture, Ready };
    HWND steps_ = nullptr;
    HWND stageTitle_ = nullptr;
    HWND stageHint_ = nullptr;
    HWND targetLabel_ = nullptr;
    HWND selectionHint_ = nullptr;
    HWND statusHeading_ = nullptr;
    HWND metrics_ = nullptr;
    HWND footer_ = nullptr;
    HWND precisionHeading_ = nullptr;
    HWND trackHeading_ = nullptr;
    HWND jumpHeading_ = nullptr;
    std::array<HWND, 4> coordinateLabels_{};
    bool advancedOpen_ = false;
    int scrollOffset_ = 0;
    int uiDpi_ = 96;
    std::wstring lastStatus_;
    std::optional<UiStage> laidOutStage_;
    bool laidOutAdvanced_ = false;
    bool laidOutHasTarget_ = false;

    static void setUiText(HWND control, const wchar_t* text) {
        if (!control) return;
        const int length = GetWindowTextLengthW(control);
        std::wstring current(static_cast<std::size_t>(length) + 1, L'\0');
        GetWindowTextW(control, current.data(), length + 1);
        current.resize(static_cast<std::size_t>(length));
        if (current != text) SetWindowTextW(control, text);
    }

    int px(int logical) const { return MulDiv(logical, uiDpi_, 96); }

    UiStage uiStage() const {
        if (capturing_ || session_.state() == SessionState::Paused ||
            session_.state() == SessionState::Capturing) return UiStage::Capture;
        if (session_.hasOutput()) return UiStage::Ready;
        return calibrationReady_ && !preview_.empty() ? UiStage::Calibrate : UiStage::Choose;
    }

    HWND addLabel(const wchar_t* text, int x, int y, int width = 180) {
        HWND label = CreateWindowExW(0, L"STATIC", text,
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
            x, y, width, 22, window_, nullptr, instance_, nullptr);
        if (bodyFont_) SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), TRUE);
        return label;
    }

    HWND addButton(const wchar_t* text, int id, int x, int y, int width = 110) {
        HWND button = CreateWindowExW(0, L"BUTTON", text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_OWNERDRAW,
            x, y, width, 36, window_, menuId(id), instance_, nullptr);
        if (bodyFont_) SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), TRUE);
        return button;
    }

    HWND addEdit(int id, int x, int y, int width = 70) {
        HWND edit = CreateWindowExW(0, L"EDIT", L"0",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER,
            x, y, width, 30, window_, menuId(id), instance_, nullptr);
        if (bodyFont_) SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), TRUE);
        return edit;
    }

    void updateFonts() {
        const HFONT oldBody = bodyFont_, oldHeading = headingFont_;
        bodyFont_ = CreateFontW(-px(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        headingFont_ = CreateFontW(-px(27), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        for (HWND child = GetWindow(window_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), TRUE);
        }
        if (stageTitle_) SendMessageW(stageTitle_, WM_SETFONT, reinterpret_cast<WPARAM>(headingFont_), TRUE);
        if (oldBody) DeleteObject(oldBody);
        if (oldHeading) DeleteObject(oldHeading);
    }

    void createControls() {
        uiDpi_ = static_cast<int>(GetDpiForWindow(window_));
        if (!uiDpi_) uiDpi_ = 96;
        updateFonts();
        BOOL dark = TRUE;
        DwmSetWindowAttribute(window_, 20, &dark, sizeof(dark));
        title_ = addLabel(L"SCROLL STITCHER", 0, 0);
        subtitle_ = addLabel(L"One continuous image. Entirely on your PC.", 0, 0);
        steps_ = addLabel(L"STEP 01 / 04  -  CHOOSE", 0, 0);
        stageTitle_ = addLabel(L"Choose a window", 0, 0);
        SendMessageW(stageTitle_, WM_SETFONT, reinterpret_cast<WPARAM>(headingFont_), TRUE);
        stageHint_ = addLabel(L"Select the app you want to capture. Its preview opens automatically.", 0, 0);
        targetLabel_ = addLabel(L"TARGET WINDOW", 0, 0);
        targetCombo_ = CreateWindowExW(0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
            0, 0, 350, 300, window_, menuId(kTargetCombo), instance_, nullptr);
        SendMessageW(targetCombo_, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), TRUE);
        SendMessageW(targetCombo_, CB_SETCUEBANNER, 0, reinterpret_cast<LPARAM>(L"Choose an open window"));
        addButton(L"Refresh", kRefresh, 0, 0);
        addButton(L"Open preview", kPreview, 0, 0);
        addButton(L"Retry preview", kRetryPreview, 0, 0);
        addButton(L"Retry detection", kAutoDetect, 0, 0);
        selectionHint_ = addLabel(L"Already have a setup? Load a saved template below.", 0, 0);
        statusHeading_ = addLabel(L"NEXT STEP", 0, 0);
        status_ = addLabel(L"Choose a window to begin.", 0, 0);
        metrics_ = addLabel(L"", 0, 0);
        addButton(L"Start capture", kStart, 0, 0);
        addButton(L"Stop capture", kStop, 0, 0);
        addButton(L"Export image", kExport, 0, 0);
        addButton(L"New capture", kNewCapture, 0, 0);
        addButton(L"Load template", kLoadTemplate, 0, 0);
        addButton(L"Save template", kSaveTemplate, 0, 0);
        addButton(L"+  Precise adjustments & settings", kAdvanced, 0, 0);
        precisionHeading_ = addLabel(L"CAPTURE AREA", 0, 0);
        trackHeading_ = addLabel(L"SCROLLBAR TRACK", 0, 0);
        const wchar_t* labels[] = {L"X", L"Y", L"Width", L"Height"};
        for (int i = 0; i < 4; ++i) {
            coordinateLabels_[i] = addLabel(labels[i], 0, 0);
            viewportEdits_[i] = addEdit(kViewportX + i, 0, 0);
            trackEdits_[i] = addEdit(kTrackX + i, 0, 0);
        }
        // The preview owns the visible candidate chooser. Keep this control as
        // its synchronized selection model for the existing command handlers.
        scrollbarCandidateCombo_ = CreateWindowExW(0, L"COMBOBOX", nullptr,
            WS_CHILD | CBS_DROPDOWNLIST, 0, 0, 1, 1, window_,
            menuId(kScrollbarCandidateCombo), instance_, nullptr);
        jumpHeading_ = addLabel(L"Maximum single-frame jump", 0, 0);
        maximumJumpSlider_ = CreateWindowExW(0, TRACKBAR_CLASSW, nullptr,
            WS_CHILD | WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS,
            0, 0, 200, 30, window_, menuId(kMaximumJump), instance_, nullptr);
        SendMessageW(maximumJumpSlider_, TBM_SETRANGE, TRUE, MAKELPARAM(kMinimumJumpPercent, kMaximumJumpPercent));
        SendMessageW(maximumJumpSlider_, TBM_SETTICFREQ, 5, 0);
        SendMessageW(maximumJumpSlider_, TBM_SETPOS, TRUE, kDefaultJumpPercent);
        maximumJumpValue_ = addLabel(L"", 0, 0);
        updateMaximumJumpLabel();
        loggingCheckbox_ = CreateWindowExW(0, L"BUTTON", L"Diagnostic logging",
            WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,
            0, 0, 200, 28, window_, menuId(kLoggingEnable), instance_, nullptr);
        SendMessageW(loggingCheckbox_, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), TRUE);
        openLogsButton_ = addButton(L"Open logs", kOpenLogs, 0, 0);
        footer_ = addLabel(L"Local capture  /  PNG + JPEG", 0, 0);
        latestReleaseLink_ = addButton(L"Help & releases", kLatestRelease, 0, 0);

        RECT mainBounds{};
        GetWindowRect(window_, &mainBounds);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor);
        const int previewWidth = std::min<int>(px(960), monitor.rcWork.right - monitor.rcWork.left);
        const int previewHeight = std::min<int>(px(760), monitor.rcWork.bottom - monitor.rcWork.top);
        const int previewX = std::clamp(mainBounds.right + 12, monitor.rcWork.left,
                                        monitor.rcWork.right - previewWidth);
        previewCanvas_ = CreateWindowExW(WS_EX_TOOLWINDOW, L"UniversalScrollStitcherPreview",
            L"Capture area - Scroll Stitcher", WS_OVERLAPPEDWINDOW,
            previewX, monitor.rcWork.top + std::max(0L, (monitor.rcWork.bottom - monitor.rcWork.top - previewHeight) / 2),
            previewWidth, previewHeight, window_, nullptr, instance_, this);
        DwmSetWindowAttribute(previewCanvas_, 20, &dark, sizeof(dark));
        refreshWorkflow();
    }

    void place(HWND control, int x, int y, int width, int height, bool visible = true) {
        if (!control) return;
        ShowWindow(control, visible ? SW_SHOWNA : SW_HIDE);
        if (visible) MoveWindow(control, px(x), px(y) - scrollOffset_, px(width), px(height), TRUE);
    }

    void placeButton(int id, int x, int y, int width, int height, bool visible = true) {
        place(GetDlgItem(window_, id), x, y, width, height, visible);
    }

    void layoutControls(int clientWidth, int clientHeight) {
        if (!stageTitle_) return;
        const int width = MulDiv(clientWidth, 96, uiDpi_);
        const int inner = std::max(400, width - 48);
        const UiStage stage = uiStage();
        const bool setup = stage == UiStage::Choose || stage == UiStage::Calibrate;
        const bool calibrated = stage == UiStage::Calibrate;
        const bool capture = stage == UiStage::Capture;
        const bool ready = stage == UiStage::Ready;
        const bool advanced = advancedOpen_ && setup;
        const int contentHeight = setup ? (advanced ? (calibrated ? 1024 : 970) : (calibrated ? 736 : 682)) : 638;
        SCROLLINFO scroll{sizeof(scroll), SIF_RANGE | SIF_PAGE | SIF_POS};
        scroll.nMax = px(contentHeight) - 1;
        scroll.nPage = static_cast<UINT>(clientHeight);
        scrollOffset_ = std::clamp(scrollOffset_, 0, std::max(0, scroll.nMax + 1 - clientHeight));
        scroll.nPos = scrollOffset_;
        SetScrollInfo(window_, SB_VERT, &scroll, TRUE);

        place(title_, 24, 24, inner, 23);
        place(subtitle_, 24, 51, inner, 25);
        place(steps_, 24, 94, inner, 24);
        place(stageTitle_, 24, 141, inner, 40);
        place(stageHint_, 24, 189, inner, 48);
        place(targetLabel_, 24, 251, inner, 22, setup);
        place(targetCombo_, 24, 280, inner - 96, 280, setup);
        placeButton(kRefresh, width - 112, 277, 88, 34, setup);
        placeButton(kPreview, 24, setup ? 329 : (ready ? 537 : 489), 140, 36, calibrated || capture || ready);
        placeButton(kRetryPreview, 24, 329, 140, 36, stage == UiStage::Choose && selectedTarget());
        placeButton(kAutoDetect, 176, 329, 145, 36, calibrated);
        place(selectionHint_, 24, 378, inner, 46, setup);
        place(statusHeading_, 24, setup ? 437 : 261, inner, 21);
        place(status_, 24, setup ? 466 : 293, inner, setup ? 56 : 64);
        place(metrics_, 24, 383, inner, 32, !setup);
        placeButton(kStart, 24, 529, inner, 44, calibrated);
        placeButton(kStop, 24, 433, inner, 44, capture);
        placeButton(kExport, 24, 433, inner, 44, ready);
        placeButton(kNewCapture, 24, 489, inner, 38, ready);
        // Setup gets a clear primary action only once a valid preview exists.
        placeButton(kLoadTemplate, 24, calibrated ? 583 : 529, (inner - 12) / 2, 34, setup);
        placeButton(kSaveTemplate, 30 + inner / 2, calibrated ? 583 : 529, (inner - 12) / 2, 34, calibrated);
        const int settingsY = calibrated ? 631 : 577;
        placeButton(kAdvanced, 24, settingsY, inner, 32, setup);

        const int detailY = settingsY + 52;
        place(precisionHeading_, 24, detailY, inner, 20, advanced);
        const int cell = (inner - 24) / 4;
        for (int i = 0; i < 4; ++i) {
            place(coordinateLabels_[i], 24 + i * (cell + 8), detailY + 28, cell, 20, advanced);
            place(viewportEdits_[i], 24 + i * (cell + 8), detailY + 54, cell, 28, advanced);
            place(trackEdits_[i], 24 + i * (cell + 8), detailY + 119, cell, 28, advanced);
        }
        place(trackHeading_, 24, detailY + 92, inner, 20, advanced);
        place(jumpHeading_, 24, detailY + 166, inner, 22, advanced);
        place(maximumJumpSlider_, 24, detailY + 194, inner - 144, 30, advanced);
        place(maximumJumpValue_, width - 152, detailY + 196, 128, 24, advanced);
        place(loggingCheckbox_, 24, detailY + 237, inner - 134, 30, advanced);
        place(openLogsButton_, width - 146, detailY + 234, 122, 34, advanced);
        const int footerY = setup ? (advanced ? detailY + 287 : settingsY + 55) : 581;
        place(footer_, 24, footerY + 7, inner - 146, 22);
        place(latestReleaseLink_, width - 165, footerY, 141, 32);
    }

    void refreshWorkflow() {
        if (!stageTitle_) return;
        const UiStage stage = uiStage();
        const bool setup = stage == UiStage::Choose || stage == UiStage::Calibrate;
        const wchar_t* heading = L"Choose a window";
        const wchar_t* hint = L"Select the app you want to capture. Its preview opens automatically.";
        const wchar_t* badge = L"NEXT STEP";
        const wchar_t* step = L"STEP 01 / 04  -  CHOOSE";
        if (stage == UiStage::Calibrate) {
            step = L"STEP 02 / 04  -  ADJUST";
            heading = L"Check your capture area";
            hint = L"Adjust the two areas in the preview. When they look right, start capturing.";
            badge = L"READY WHEN YOU ARE";
        } else if (stage == UiStage::Capture) {
            step = L"STEP 03 / 04  -  CAPTURE";
            heading = capturing_ ? (recovering_ ? L"Reconnect your capture" : L"Capture in progress") : L"Capture paused";
            hint = L"Switch to your target window and scroll down. Stop when the last content is visible.";
            badge = recovering_ ? L"RECOVERING" : (capturing_ ? L"CAPTURING" : L"YOUR CAPTURE IS SAFE");
        } else if (stage == UiStage::Ready) {
            step = L"STEP 04 / 04  -  SAVE";
            heading = exported_ ? L"Your image is saved" : L"Capture ready";
            hint = L"Save a continuous image. PNG keeps text sharp; JPEG is available in the save dialog.";
            badge = exported_ ? L"SAVED" : L"READY TO EXPORT";
        }
        setUiText(stageTitle_, heading);
        setUiText(steps_, step);
        setUiText(stageHint_, hint);
        setUiText(statusHeading_, badge);
        setUiText(selectionHint_, stage == UiStage::Choose
            ? L"Already have a setup? Load a saved template below."
            : (validCalibration() ? L"Use Capture area or Scrollbar in the preview. Zoom with the wheel; hold Space to pan."
                : L"Keep both areas inside the image and select the scrollbar's full travel height."));
        setUiText(GetDlgItem(window_, kAdvanced), advancedOpen_
            ? L"-  Precise adjustments & settings" : L"+  Precise adjustments & settings");
        setUiText(GetDlgItem(window_, kStop), capturing_ ? L"Stop capture" : L"Finish & keep capture");
        const std::wstring dimensions = session_.outputRows() > 0
            ? std::to_wstring(session_.viewport().width) + L" x " + std::to_wstring(session_.outputRows()) + L" px captured"
            : L"Waiting for the first scroll";
        setUiText(metrics_, dimensions.c_str());
        EnableWindow(targetCombo_, setup);
        EnableWindow(GetDlgItem(window_, kRefresh), setup);
        EnableWindow(GetDlgItem(window_, kAutoDetect), stage == UiStage::Calibrate);
        EnableWindow(GetDlgItem(window_, kLoadTemplate), setup);
        EnableWindow(GetDlgItem(window_, kSaveTemplate), stage == UiStage::Calibrate);
        EnableWindow(GetDlgItem(window_, kStart), stage == UiStage::Calibrate && validCalibration());
        EnableWindow(maximumJumpSlider_, setup);
        for (HWND edit : viewportEdits_) EnableWindow(edit, stage == UiStage::Calibrate);
        for (HWND edit : trackEdits_) EnableWindow(edit, stage == UiStage::Calibrate);
        updatePreviewToolButtons();
        const bool hasTarget = selectedTarget() != nullptr;
        if (!laidOutStage_ || *laidOutStage_ != stage || laidOutAdvanced_ != advancedOpen_ || laidOutHasTarget_ != hasTarget) {
            laidOutStage_ = stage;
            laidOutAdvanced_ = advancedOpen_;
            laidOutHasTarget_ = hasTarget;
            RECT client{};
            GetClientRect(window_, &client);
            layoutControls(client.right, client.bottom);
        }
    }

    bool validCalibration() const {
        if (preview_.empty()) return false;
        const Rect content = readRect(viewportEdits_), track = readRect(trackEdits_);
        const auto inside = [&](const Rect& r) {
            return r.valid() && r.x >= 0 && r.y >= 0 &&
                r.right() <= preview_.cols && r.bottom() <= preview_.rows;
        };
        const int movingHeight = std::min(content.bottom(), track.bottom()) - std::max(content.y, track.y);
        // Match the core's useful-band guard so the preview's header/exclusion
        // labels always describe the image that will actually be exported.
        return inside(content) && inside(track) && movingHeight >= std::max(32, content.height / 5);
    }

    void scrollControls(int action, int position = 0) {
        SCROLLINFO info{sizeof(info), SIF_ALL};
        GetScrollInfo(window_, SB_VERT, &info);
        switch (action) {
        case SB_LINEUP: scrollOffset_ -= px(35); break;
        case SB_LINEDOWN: scrollOffset_ += px(35); break;
        case SB_PAGEUP: scrollOffset_ -= static_cast<int>(info.nPage); break;
        case SB_PAGEDOWN: scrollOffset_ += static_cast<int>(info.nPage); break;
        case SB_THUMBTRACK: scrollOffset_ = info.nTrackPos; break;
        case SB_THUMBPOSITION: scrollOffset_ = position; break;
        default: return;
        }
        RECT client{};
        GetClientRect(window_, &client);
        layoutControls(client.right, client.bottom);
        InvalidateRect(window_, nullptr, TRUE);
    }

    LRESULT colorStatic(HWND control, HDC dc) const {
        SetBkMode(dc, TRANSPARENT);
        const bool muted = control == subtitle_ || control == stageHint_ || control == selectionHint_ ||
            control == footer_ || control == targetLabel_ || control == precisionHeading_ || control == trackHeading_;
        SetTextColor(dc, muted ? RGB(159, 171, 188) : RGB(235, 240, 248));
        if (control == steps_ || control == statusHeading_) SetTextColor(dc, RGB(132, 181, 255));
        return reinterpret_cast<LRESULT>(backgroundBrush_);
    }

    void drawButton(const DRAWITEMSTRUCT* item) const {
        if (!item || item->CtlType != ODT_BUTTON) return;
        const bool primary = item->CtlID == kStart || item->CtlID == kExport || item->CtlID == kStop;
        COLORREF fill = primary ? RGB(117, 171, 255) : RGB(34, 41, 53);
        COLORREF text = primary ? RGB(14, 29, 51) : RGB(225, 233, 246);
        if (item->itemState & ODS_DISABLED) {
            fill = RGB(30, 35, 43); text = RGB(109, 120, 137);
        } else if (item->itemState & ODS_SELECTED) {
            fill = primary ? RGB(89, 145, 228) : RGB(51, 62, 80);
        }
        RECT rect = item->rcItem;
        InflateRect(&rect, -1, -1);
        HBRUSH brush = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_SOLID, 1, primary ? fill : RGB(54, 64, 80));
        HGDIOBJ oldBrush = SelectObject(item->hDC, brush), oldPen = SelectObject(item->hDC, pen);
        RoundRect(item->hDC, rect.left, rect.top, rect.right, rect.bottom, px(10), px(10));
        SelectObject(item->hDC, oldPen); SelectObject(item->hDC, oldBrush);
        DeleteObject(pen); DeleteObject(brush);
        HGDIOBJ oldFont = SelectObject(item->hDC, bodyFont_);
        SetBkMode(item->hDC, TRANSPARENT);
        SetTextColor(item->hDC, text);
        wchar_t caption[128]{};
        GetWindowTextW(item->hwndItem, caption, static_cast<int>(std::size(caption)));
        DrawTextW(item->hDC, caption, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(item->hDC, oldFont);
        if (item->itemState & ODS_FOCUS) {
            InflateRect(&rect, -4, -4);
            DrawFocusRect(item->hDC, &rect);
        }
    }
