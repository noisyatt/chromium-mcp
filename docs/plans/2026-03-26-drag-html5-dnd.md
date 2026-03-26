# Drag HTML5 DnD 지원 구현 계획

## 배경

현재 `drag_tool.cc`는 `Input.dispatchMouseEvent`(mousePressed/mouseMoved/mouseReleased)만 사용.
이 방식은 OS 레벨 DnD 제스처 인식을 우회하여 HTML5 `dragstart`/`dragover`/`drop` 이벤트가 발생하지 않음.

Chromium CDP에는 `Input.setInterceptDrags` + `Input.dispatchDragEvent` API가 존재하며,
Playwright가 이를 production에서 사용 중.

## 새 CDP 시퀀스 (Playwright 패턴)

```
[현재]                              [변경 후]
mousePressed(start)                 mousePressed(start)
                                    Input.setInterceptDrags(enabled:true)
mouseMoved × steps                  mouseMoved × steps
                                      ↳ Input.dragIntercepted 이벤트로 DragData 캡처
                                    Input.setInterceptDrags(enabled:false)
                                    Input.dispatchDragEvent(dragEnter, end, data)
                                    Input.dispatchDragEvent(dragOver, end, data)
mouseReleased(end)                  Input.dispatchDragEvent(drop, end, data)
                                    mouseReleased(end)
```

## 구현 태스크

### Task 1: DragContext 구조체 추가 (drag_tool.h)

드래그 시퀀스 전체에서 공유할 상태를 담는 구조체:

```cpp
struct DragContext {
  double start_x, start_y, end_x, end_y;
  int steps;
  McpSession* session;
  base::OnceCallback<void(base::Value)> callback;
  // HTML5 DnD 지원
  bool drag_intercepted = false;
  base::DictValue drag_data;  // Input.dragIntercepted에서 캡처한 DragData
};
```

기존 함수 시그니처들의 6개 파라미터(start_x, start_y, end_x, end_y, steps, session)를 `shared_ptr<DragContext>`로 통합.

### Task 2: setInterceptDrags 호출 추가 (drag_tool.cc)

`OnMousePressed` 콜백에서:
1. `Input.dragIntercepted` 이벤트 핸들러를 `session->RegisterCdpEventHandler()`로 등록
2. `Input.setInterceptDrags(enabled:true)` CDP 명령 전송
3. 응답 수신 후 기존 `DispatchNextMove` 호출

### Task 3: dragIntercepted 이벤트 캡처 (drag_tool.cc)

이벤트 핸들러에서:
- `params`의 `"data"` 필드(DragData)를 `ctx->drag_data`에 저장
- `ctx->drag_intercepted = true` 플래그 설정

### Task 4: dispatchDragEvent 시퀀스 추가 (drag_tool.cc)

`DispatchMouseReleased` 대신 새 `DispatchDragEvents` 함수:
1. `Input.setInterceptDrags(enabled:false)` 전송
2. `drag_intercepted == true`이면:
   - `Input.dispatchDragEvent(type:"dragEnter", x:end_x, y:end_y, data:drag_data)`
   - `Input.dispatchDragEvent(type:"dragOver", x:end_x, y:end_y, data:drag_data)`
   - `Input.dispatchDragEvent(type:"drop", x:end_x, y:end_y, data:drag_data)`
3. `drag_intercepted == false`이면 (요소가 draggable이 아닌 경우):
   - DragEvent 단계 건너뛰고 바로 mouseReleased (기존 동작 유지)
4. 마지막에 `mouseReleased(end)` 전송
5. 이벤트 핸들러 해제: `session->UnregisterCdpEventHandler("Input.dragIntercepted")`

### Task 5: 폴백 DragData (drag_tool.cc)

`dragIntercepted` 이벤트가 오지 않는 경우를 위한 폴백:
- 빈 DragData: `{"items": [], "dragOperationsMask": 65535}`
- 이 데이터로 dispatchDragEvent를 호출하면 이벤트는 발화되지만 dataTransfer.items는 비어있음
- 대부분의 UI 라이브러리(SortableJS, react-beautiful-dnd)는 이벤트 발화만으로 동작

### Task 6: E2E 테스트 검증

테스트 페이지의 기존 drag 테스트 실행:
- `drag(startSelector:"#drag-source", endSelector:"#drop-target")`
- `evaluate('document.getElementById("drop-target").textContent')` → "Dropped!" 확인

## 수정 파일

| 파일 | 변경 |
|------|------|
| `src/tools/drag_tool.h` | DragContext 구조체, 새 콜백 메서드 선언 |
| `src/tools/drag_tool.cc` | 전체 시퀀스 재구현 |

## 비고

- `Input.setInterceptDrags`/`Input.dispatchDragEvent`는 Experimental API
- Chromium 146에서 사용 가능 확인 필요 (빌드 시 확인됨)
- 기존 좌표 모드/로케이터 모드 인터페이스는 변경 없음
