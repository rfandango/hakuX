package com.rfandango.xemuandroid

import android.content.Context
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.util.TypedValue
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView

class PauseMenuOverlay(context: Context) : FrameLayout(context) {

    var onExitEmulation: (() -> Unit)? = null
    var onDismiss: (() -> Unit)? = null

    init {
        setBackgroundColor(Color.argb(160, 0, 0, 0))
        visibility = View.GONE
        isClickable = true
        isFocusable = true

        val card = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
            val bg = GradientDrawable().apply {
                setColor(Color.argb(230, 30, 30, 30))
                cornerRadius = dpToPx(16f)
            }
            background = bg
            setPadding(dpToPx(32f).toInt(), dpToPx(24f).toInt(), dpToPx(32f).toInt(), dpToPx(24f).toInt())
            elevation = dpToPx(8f)
        }

        val titleText = TextView(context).apply {
            text = "Emulation Menu"
            setTextColor(Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 20f)
            typeface = Typeface.DEFAULT_BOLD
            gravity = Gravity.CENTER
        }
        card.addView(titleText, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT,
            LinearLayout.LayoutParams.WRAP_CONTENT
        ).apply { bottomMargin = dpToPx(24f).toInt() })

        val exitButton = createMenuButton(context, "Exit Emulation").apply {
            val bg = (background as GradientDrawable)
            bg.setColor(Color.argb(200, 180, 40, 40))
            setOnClickListener { onExitEmulation?.invoke() }
        }
        card.addView(exitButton, createButtonParams())

        val cardParams = LayoutParams(
            dpToPx(280f).toInt(),
            LayoutParams.WRAP_CONTENT
        ).apply { gravity = Gravity.CENTER }
        addView(card, cardParams)

        setOnTouchListener { _, event ->
            if (event.action == MotionEvent.ACTION_UP && visibility == View.VISIBLE) {
                onDismiss?.invoke()
                true
            } else {
                true
            }
        }
    }

    fun show() {
        visibility = View.VISIBLE
    }

    fun dismiss() {
        visibility = View.GONE
    }

    fun isShowing(): Boolean = visibility == View.VISIBLE

    private fun createMenuButton(context: Context, label: String): Button {
        return Button(context).apply {
            text = label
            setTextColor(Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f)
            typeface = Typeface.DEFAULT_BOLD
            isAllCaps = false
            val bg = GradientDrawable().apply {
                setColor(Color.argb(200, 70, 70, 70))
                cornerRadius = dpToPx(8f)
            }
            background = bg
            setPadding(dpToPx(16f).toInt(), dpToPx(14f).toInt(), dpToPx(16f).toInt(), dpToPx(14f).toInt())
            stateListAnimator = null
        }
    }

    private fun createButtonParams(): LinearLayout.LayoutParams {
        return LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
            LinearLayout.LayoutParams.WRAP_CONTENT
        ).apply { bottomMargin = dpToPx(12f).toInt() }
    }

    private fun dpToPx(dp: Float): Float {
        return TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, dp, resources.displayMetrics)
    }
}
