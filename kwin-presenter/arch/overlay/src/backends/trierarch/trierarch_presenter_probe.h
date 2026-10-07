#pragma once

#include <QString>

namespace KWin
{

class TrierarchPresenterProbe
{
public:
    TrierarchPresenterProbe() = default;
    ~TrierarchPresenterProbe();

    bool connectTo(const QString &socketPath, QString *error);

private:
    int m_fd = -1;
};

} // namespace KWin
