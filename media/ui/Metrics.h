#ifndef METRICS_H
#define METRICS_H

namespace media {

/*!
 * \brief 统一的设计尺寸规范。
 *
 * 审计发现的问题：圆角在 QSS 里有 10 种取值（2/4/5/6/7/9/10/12/14px），
 * 字号有 8 种，控件高度有 5 种。同一类控件在不同页面里长得不一样，
 * 用户说不出哪里不对，但会觉得"这个软件不精致"。
 *
 * 这个文件把所有尺寸收敛到一套语义化档位。规则只有两条：
 *
 *   1. 同类控件用同一档位，不各自取值。
 *      按钮、输入框、列表行、卡片、对话框分别对应固定的
 *      Radius::* 和 Control::Height*，写代码时按语义取，不现场编数字。
 *
 *   2. 间距只用 Space::* 的 4/8/12/16/24。
 *      这些值互为倍数，任意组合都不会出现 13px、17px 这类"差一点对齐"的间隙。
 *      例外只有 Control::Separator（1px 分隔线），它表示"结构边界"而非"间隙"。
 *
 * 颜色不在这里 —— 颜色已经在 ThemeManager 里按语义集中管理，不需要第二处。
 */

namespace Metrics {

/*! 圆角。按控件尺寸分档：小控件用小圆角，大容器用大圆角。 */
namespace Radius {
    constexpr int Control = 6;    //!< 按钮、输入框、下拉框等交互控件
    constexpr int Chip    = 12;   //!< 标签 chip、胶囊形控件（半径约为高度一半）
    constexpr int Card    = 10;   //!< 卡片、面板内的内容容器
    constexpr int Frame   = 14;   //!< 对话框、浮层等大容器
}

/*! 控件高度。统一后同一行内的控件天然等高，不需要逐个 setFixedHeight。 */
namespace Control {
    constexpr int Button  = 28;   //!< 常规按钮与输入框
    constexpr int IconButton = 28;  //!< 工具条图标按钮（与常规按钮同高，保证对齐）
    constexpr int ComboBox = 28;
    constexpr int ListRow = 32;   //!< 列表行、导航项
    constexpr int Separator = 1;  //!< 分隔线厚度
}

/*! 间距。只用这几个值，保证任意嵌套后边界仍然对齐。 */
namespace Space {
    constexpr int XS  = 4;
    constexpr int SM  = 8;
    constexpr int MD  = 12;
    constexpr int LG  = 16;
    constexpr int XL  = 24;
}

/*! 字号。五档够用，避免每个标签自定一个字号。 */
namespace Font {
    constexpr int Caption = 11;   //!< 辅助说明、键名、时间戳
    constexpr int Body    = 12;   //!< 正文、列表项、chip
    constexpr int Strong  = 13;   //!< 需要强调的正文、文件名
    constexpr int Title   = 14;   //!< 区块标题、对话框标题
    constexpr int Heading = 20;   //!< 页面大标题（首页"资源库概览"）
    constexpr int Stat    = 24;   //!< 统计卡的大数字
}

/*! 常用面板宽度。 */
namespace Panel {
    constexpr int SidebarMin = 180;
    constexpr int SidebarMax = 360;
    constexpr int DetailMin  = 260;
    constexpr int DetailMax  = 420;
}

} // namespace Metrics

} // namespace media

#endif // METRICS_H
