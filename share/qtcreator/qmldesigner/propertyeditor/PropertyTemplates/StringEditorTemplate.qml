QWidget {
    layout: HorizontalLayout {
        Label {
            text: "%1"
            toolTip: "%1"
         }
         LineEdit {
             backendValue: backendValues.%2
             baseStateFlag: isBaseState
         }
    }
}