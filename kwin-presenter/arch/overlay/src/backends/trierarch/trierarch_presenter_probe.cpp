#include "trierarch_presenter_probe.h"

#include "trierarch_presenter_protocol.h"

#include <QByteArray>

#include <cerrno>
#include <cstring>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace KWin
{

TrierarchPresenterProbe::~TrierarchPresenterProbe()
{
    if (m_fd >= 0) {
        close(m_fd);
    }
}

bool TrierarchPresenterProbe::connectTo(const QString &socketPath, QString *error)
{
    const QByteArray path = socketPath.toLocal8Bit();
    sockaddr_un address = {};
    if (path.isEmpty() || path.size() >= static_cast<int>(sizeof(address.sun_path))) {
        if (error) {
            *error = QStringLiteral("presenter socket path is invalid");
        }
        return false;
    }

    const int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        if (error) {
            *error = QString::fromLocal8Bit(std::strerror(errno));
        }
        return false;
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.constData(), path.size() + 1);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
        if (error) {
            *error = QString::fromLocal8Bit(std::strerror(errno));
        }
        close(fd);
        return false;
    }

    const trierarch_presenter_hello hello = {
        .header = {
            .magic = TRIERARCH_PRESENTER_MAGIC,
            .version = TRIERARCH_PRESENTER_VERSION,
            .type = TRIERARCH_PRESENTER_HELLO,
        },
    };
    if (send(fd, &hello, sizeof(hello), MSG_NOSIGNAL) !=
            static_cast<ssize_t>(sizeof(hello))) {
        if (error) {
            *error = QString::fromLocal8Bit(std::strerror(errno));
        }
        close(fd);
        return false;
    }
    m_fd = fd;
    return true;
}

} // namespace KWin
